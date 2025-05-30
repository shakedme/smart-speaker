//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_AUDIO_MIXER_H
#define BT_SPEAKER_AUDIO_MIXER_H


#pragma once

#include "audio_types.hpp"
#include "audio_buffer.hpp"
#include "event_bus.hpp"
#include "i2s_driver.hpp"
#include <array>
#include <memory>
#include <atomic>

extern "C" {
#include "esp_attr.h"
#include "esp_timer.h"
}

namespace smart_speaker {

// Audio source management for mixing
    // Audio source management for mixing - Fixed member initialization order
    struct AudioSourceInfo {
        std::unique_ptr<AudioFrameBuffer> buffer;
        std::atomic<float> volume{1.0f};
        std::atomic<uint8_t> priority{static_cast<uint8_t>(AudioPriority::Music)};
        std::atomic<bool> active{false};
        std::atomic<bool> muted{false};
        AudioSource source_id{AudioSource::Invalid};

        // Performance tracking
        std::atomic<uint32_t> frames_mixed{0};
        std::atomic<uint32_t> buffer_underruns{0};

        AudioSourceInfo() = default;

        // Initialize with source type - Fixed initialization order
        explicit AudioSourceInfo(AudioSource id, AudioPriority prio = AudioPriority::Music)
                : priority(static_cast<uint8_t>(prio)),  // Initialize in declaration order
                  source_id(id) {                        // source_id comes after priority
            buffer = AudioBufferFactory::create_audio_frame_buffer();
            active.store(true, std::memory_order_release);
        }

        // Non-copyable, movable
        AudioSourceInfo(const AudioSourceInfo &) = delete;

        AudioSourceInfo &operator=(const AudioSourceInfo &) = delete;

        AudioSourceInfo(AudioSourceInfo &&) = default;

        AudioSourceInfo &operator=(AudioSourceInfo &&) = default;

        // Control interface
        void set_volume(float vol) {
            volume.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed);
        }

        void set_priority(AudioPriority prio) {
            priority.store(static_cast<uint8_t>(prio), std::memory_order_relaxed);
        }

        void set_active(bool is_active) {
            active.store(is_active, std::memory_order_release);
        }

        void set_muted(bool is_muted) {
            muted.store(is_muted, std::memory_order_relaxed);
        }

        // Status queries
        float get_volume() const { return volume.load(std::memory_order_relaxed); }

        uint8_t get_priority() const { return priority.load(std::memory_order_relaxed); }

        bool is_active() const { return active.load(std::memory_order_acquire); }

        bool is_muted() const { return muted.load(std::memory_order_relaxed); }

        // Effective volume (considering mute state)
        float effective_volume() const {
            return is_muted() ? 0.0f : get_volume();
        }
    };

// Real-time audio mixer with priority-based mixing
    template<size_t MaxSources = 4>
    class AudioMixer {
    private:
        std::array<AudioSourceInfo, MaxSources> sources_;
        std::unique_ptr<I2SDriver> i2s_driver_;

        // Mix buffers (32-bit for headroom)
        alignas(32) std::array<Sample32, AudioConfig::FRAME_SIZE> mix_buffer_32_;
        alignas(32) StandardFrame output_frame_;

        // Mixing state
        std::atomic<float> master_volume_{1.0f};
        std::atomic<bool> master_muted_{false};
        std::atomic<bool> running_{false};

        // Performance metrics
        mutable std::atomic<uint32_t> frames_processed_{0};
        mutable std::atomic<uint32_t> mix_overflows_{0};
        mutable std::atomic<uint32_t> max_mix_time_us_{0};

        static constexpr const char *TAG = "AUDIO_MIXER";

    public:
        explicit AudioMixer(std::unique_ptr<I2SDriver> driver)
                : i2s_driver_(std::move(driver)) {

            // Initialize sources as inactive
            for (size_t i = 0; i < MaxSources; ++i) {
                sources_[i].set_active(false);
            }
        }

        ~AudioMixer() {
            stop();
        }

        // Non-copyable, movable
        AudioMixer(const AudioMixer &) = delete;

        AudioMixer &operator=(const AudioMixer &) = delete;

        AudioMixer(AudioMixer &&) = default;

        AudioMixer &operator=(AudioMixer &&) = default;

        // Lifecycle management
        AudioError initialize() {
            if (!i2s_driver_) {
                return AudioError::ConfigError;
            }

            AudioError result = i2s_driver_->initialize();
            if (result != AudioError::OK) {
                ESP_LOGE(TAG, "Failed to initialize I2S driver");
                return result;
            }

            return AudioError::OK;
        }

        AudioError start() {
            if (!i2s_driver_ || !i2s_driver_->is_initialized()) {
                return AudioError::ConfigError;
            }

            AudioError result = i2s_driver_->start();
            if (result != AudioError::OK) {
                ESP_LOGE(TAG, "Failed to start I2S driver");
                return result;
            }

            running_.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "Audio mixer started");

            return AudioError::OK;
        }

        void stop() {
            running_.store(false, std::memory_order_release);

            if (i2s_driver_) {
                i2s_driver_->stop();
            }

            ESP_LOGI(TAG, "Audio mixer stopped");
        }

        // Source management
        bool add_source(AudioSource source_id, AudioPriority priority = AudioPriority::Music) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources) {
                ESP_LOGE(TAG, "Invalid source index: %zu", index);
                return false;
            }

            sources_[index] = AudioSourceInfo(source_id, priority);

            ESP_LOGI(TAG, "Added audio source: %d with priority %d",
                     static_cast<int>(source_id), static_cast<int>(priority));

            return true;
        }

        bool remove_source(AudioSource source_id) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources) {
                return false;
            }

            sources_[index].set_active(false);
            sources_[index].buffer.reset();

            ESP_LOGI(TAG, "Removed audio source: %d", static_cast<int>(source_id));
            return true;
        }

        // Audio processing (real-time safe)
        IRAM_ATTR AudioError process_frame() {
            if (!running_.load(std::memory_order_acquire)) {
                return AudioError::ConfigError;
            }

            uint32_t start_time = esp_timer_get_time();

            // Clear mix buffer
            mix_buffer_32_.fill(0);

            // Mix all active sources by priority
            mix_sources_by_priority();

            // Apply master volume and convert to output format
            apply_master_effects();

            // Output to I2S
            AudioError result = i2s_driver_->write_frame(output_frame_);

            // Update performance metrics
            update_performance_metrics(start_time);

            return result;
        }

        // Source control interface
        bool set_source_volume(AudioSource source_id, float volume) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources) return false;

            sources_[index].set_volume(volume);

            // Publish volume change event
            VolumeChangeData data{source_id, sources_[index].get_volume(), volume, sources_[index].is_muted()};
            PUBLISH_EVENT(EventType::AudioVolumeChanged, data);

            return true;
        }

        bool set_source_active(AudioSource source_id, bool active) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources) return false;

            sources_[index].set_active(active);

            if (active) {
                PUBLISH_EVENT(EventType::AudioSourceConnected, source_id);
            } else {
                PUBLISH_EVENT(EventType::AudioSourceDisconnected, source_id);
            }

            return true;
        }

        bool set_source_muted(AudioSource source_id, bool muted) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources) return false;

            sources_[index].set_muted(muted);
            return true;
        }

        // Master volume control
        void set_master_volume(float volume) {
            master_volume_.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
        }

        void set_master_muted(bool muted) {
            master_muted_.store(muted, std::memory_order_relaxed);
        }

        float get_master_volume() const {
            return master_volume_.load(std::memory_order_relaxed);
        }

        bool is_master_muted() const {
            return master_muted_.load(std::memory_order_relaxed);
        }

        // Buffer access for audio sources
        AudioFrameBuffer *get_source_buffer(AudioSource source_id) {
            size_t index = static_cast<size_t>(source_id);
            if (index >= MaxSources || !sources_[index].is_active()) {
                return nullptr;
            }

            return sources_[index].buffer.get();
        }

        // Performance monitoring
        AudioMetrics get_metrics() const {
            AudioMetrics metrics;
            metrics.frames_processed = frames_processed_.load(std::memory_order_relaxed);
            metrics.buffer_underruns = get_total_underruns();
            metrics.buffer_overruns = mix_overflows_.load(std::memory_order_relaxed);
            metrics.max_processing_time_us = max_mix_time_us_.load(std::memory_order_relaxed);

            if (i2s_driver_) {
                metrics.frames_processed += i2s_driver_->get_write_count();
            }

            return metrics;
        }

        void reset_metrics() {
            frames_processed_.store(0, std::memory_order_relaxed);
            mix_overflows_.store(0, std::memory_order_relaxed);
            max_mix_time_us_.store(0, std::memory_order_relaxed);

            for (auto &source: sources_) {
                source.frames_mixed.store(0, std::memory_order_relaxed);
                source.buffer_underruns.store(0, std::memory_order_relaxed);
            }

            if (i2s_driver_) {
                i2s_driver_->reset_metrics();
            }
        }

        bool is_running() const {
            return running_.load(std::memory_order_acquire);
        }

    private:
        IRAM_ATTR void mix_sources_by_priority() {
            // Sort sources by priority (higher priority mixed last = on top)
            std::array<size_t, MaxSources> source_order;
            for (size_t i = 0; i < MaxSources; ++i) {
                source_order[i] = i;
            }

            // Simple priority-based sorting (bubble sort for small arrays)
            for (size_t i = 0; i < MaxSources - 1; ++i) {
                for (size_t j = 0; j < MaxSources - i - 1; ++j) {
                    if (sources_[source_order[j]].get_priority() >
                        sources_[source_order[j + 1]].get_priority()) {
                        std::swap(source_order[j], source_order[j + 1]);
                    }
                }
            }

            // Mix sources in priority order
            for (size_t i = 0; i < MaxSources; ++i) {
                mix_single_source(source_order[i]);
            }
        }

        IRAM_ATTR void mix_single_source(size_t source_index) {
            auto &source = sources_[source_index];

            if (!source.is_active() || !source.buffer) {
                return;
            }

            // Try to read a frame from the source buffer
            const StandardFrame *frame = source.buffer->peek_read(1);
            if (!frame) {
                source.buffer_underruns.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const float effective_vol = source.effective_volume();

            // Mix samples with volume control
            for (size_t i = 0; i < AudioConfig::FRAME_SIZE; ++i) {
                Sample32 sample = static_cast<Sample32>(frame->samples[i] * effective_vol);
                mix_buffer_32_[i] += sample;
            }

            source.buffer->commit_read(1);
            source.frames_mixed.fetch_add(1, std::memory_order_relaxed);
        }

        IRAM_ATTR void apply_master_effects() {
            const float master_vol = is_master_muted() ? 0.0f : get_master_volume();

            for (size_t i = 0; i < AudioConfig::FRAME_SIZE; ++i) {
                // Apply master volume and clamp to 16-bit range
                Sample32 sample = static_cast<Sample32>(mix_buffer_32_[i] * master_vol);

                // Saturation with overflow detection
                if (sample > 32767) {
                    sample = 32767;
                    mix_overflows_.fetch_add(1, std::memory_order_relaxed);
                } else if (sample < -32768) {
                    sample = -32768;
                    mix_overflows_.fetch_add(1, std::memory_order_relaxed);
                }

                output_frame_.samples[i] = static_cast<Sample16>(sample);
            }

            // Set frame metadata
            output_frame_.timestamp_us = esp_timer_get_time();
            output_frame_.source = AudioSource::System;  // Mixed output
            output_frame_.channels = AudioConfig::CHANNELS;
            output_frame_.sample_rate = AudioConfig::SAMPLE_RATE;
        }

        IRAM_ATTR void update_performance_metrics(uint32_t start_time) {
            uint32_t processing_time = esp_timer_get_time() - start_time;

            // Update max processing time
            uint32_t current_max = max_mix_time_us_.load(std::memory_order_relaxed);
            while (processing_time > current_max) {
                if (max_mix_time_us_.compare_exchange_weak(current_max, processing_time,
                                                           std::memory_order_relaxed)) {
                    break;
                }
            }

            frames_processed_.fetch_add(1, std::memory_order_relaxed);
        }

        uint32_t get_total_underruns() const {
            uint32_t total = 0;
            for (const auto &source: sources_) {
                total += source.buffer_underruns.load(std::memory_order_relaxed);
            }
            return total;
        }
    };

// Type alias for the standard mixer configuration
    using StandardAudioMixer = AudioMixer<4>;

} // namespace smart_speaker


#endif //BT_SPEAKER_AUDIO_MIXER_H
