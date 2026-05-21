//
// Created by Shaked Melman on 17/06/2025.
//


#include "../include/adf_audio_sink.hpp"
#include "../include/audio_pipeline_util.hpp"
#include "diagnostics_config.hpp"
#include <algorithm>
#include <stdexcept>

extern "C" {
#include "audio_common.h"
#include "ringbuf.h"
#include "i2s_stream.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    namespace {
        constexpr int kPlaybackDiagIntervalMs = 5000;
        constexpr uint32_t kRingbufBlockWarnMs = 50;
        constexpr uint32_t kRingbufMaxBackoffMs = 10;
        constexpr uint32_t kFeedBackpressureReturnMs = 25;

        size_t ringbufFilled(ringbuf_handle_t rb) {
            return rb ? rb_bytes_filled(rb) : 0;
        }

        int i2sElementState(const ElementHandle &element) {
            return element ? static_cast<int>(audio_element_get_state(element.get())) : -1;
        }
    }

    void ADFAudioSink::maybeLogResumeFollowUp() {
        if (!diagnostics_enabled || resumeDiagStartUs_ == 0 || !audioPipeline) {
            return;
        }

        const int64_t elapsed_ms = (esp_timer_get_time() - resumeDiagStartUs_) / 1000;
        const size_t fill = getBufferFillLevel();
        const size_t capacity = getBufferCapacity();
        const int i2s_state = i2sElementState(audioPipeline->i2SStreamWriter);

        if (!resumeDiag500Logged_ && elapsed_ms >= 500) {
            resumeDiag500Logged_ = true;
            ESP_LOGI(TAG,
                     "[PLAYBACK_RESUME_DIAG] +500ms fill=%u/%u i2s_state=%d playing=%d",
                     static_cast<unsigned>(fill),
                     static_cast<unsigned>(capacity),
                     i2s_state,
                     isPlaying_.load() ? 1 : 0);
        }

        if (!resumeDiag2000Logged_ && elapsed_ms >= 2000) {
            resumeDiag2000Logged_ = true;
            resumeDiagStartUs_ = 0;
            ESP_LOGI(TAG,
                     "[PLAYBACK_RESUME_DIAG] +2000ms fill=%u/%u i2s_state=%d playing=%d",
                     static_cast<unsigned>(fill),
                     static_cast<unsigned>(capacity),
                     i2s_state,
                     isPlaying_.load() ? 1 : 0);
        }
    }

    void ADFAudioSink::maybeLogPlaybackDiagnostics() {
        if (!diagnostics_enabled) {
            return;
        }

        const int64_t now_us = esp_timer_get_time();
        if (lastDiagLogUs_ != 0 &&
            (now_us - lastDiagLogUs_) < (kPlaybackDiagIntervalMs * 1000LL)) {
            return;
        }
        lastDiagLogUs_ = now_us;

        const uint32_t blocks = ringbufBlockCount_.exchange(0, std::memory_order_relaxed);
        const uint32_t max_wait = ringbufBlockMaxWaitMs_.exchange(0, std::memory_order_relaxed);
        const uint32_t short_writes = feedShortWriteCount_.exchange(0, std::memory_order_relaxed);
        const size_t min_fill = minFillBytes_.exchange(SIZE_MAX, std::memory_order_relaxed);
        const size_t fill = getBufferFillLevel();
        const size_t capacity = getBufferCapacity();

        ESP_LOGI(TAG,
                 "[PLAYBACK_DIAG] fill=%u/%u min_fill=%u rb_block=%lu rb_block_max_ms=%lu short_write=%lu playing=%d",
                 static_cast<unsigned>(fill),
                 static_cast<unsigned>(capacity),
                 static_cast<unsigned>(min_fill == SIZE_MAX ? 0 : min_fill),
                 static_cast<unsigned long>(blocks),
                 static_cast<unsigned long>(max_wait),
                 static_cast<unsigned long>(short_writes),
                 isPlaying_.load() ? 1 : 0);
    }

    ADFAudioSink::ADFAudioSink() :
            pipelineRunning_(false),
            initialized_(false) {
        ESP_LOGI(TAG, "ADFAudioSink created");
    }

    ADFAudioSink::~ADFAudioSink() {
        ESP_LOGI(TAG, "ADFAudioSink destructor called");
    }

    size_t ADFAudioSink::feedPCMFrames(const uint8_t *buffer, size_t bytes) {
        if (!initialized_ || !buffer || bytes == 0) {
            return 0;
        }

        size_t offset = 0;
        auto raw = audioPipeline->i2SStreamReader.get();
        uint32_t backoffMs = 1;
        const int64_t feed_start_us = esp_timer_get_time();

        while (offset < bytes) {
            size_t toWrite = std::min(kWriteChunkSize, bytes - offset);

            ringbuf_handle_t rb = audio_element_get_output_ringbuf(raw);
            size_t free_bytes = rb ? rb_bytes_available(rb) : 0;
            if (free_bytes == 0) {
                const int64_t block_start_us = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(backoffMs));
                const uint32_t waited_ms = static_cast<uint32_t>(
                        (esp_timer_get_time() - block_start_us) / 1000);
                if (diagnostics_enabled) {
                    ringbufBlockCount_.fetch_add(1, std::memory_order_relaxed);
                    uint32_t prev_max = ringbufBlockMaxWaitMs_.load(std::memory_order_relaxed);
                    while (waited_ms > prev_max &&
                           !ringbufBlockMaxWaitMs_.compare_exchange_weak(
                                   prev_max, waited_ms, std::memory_order_relaxed)) {
                    }
                    if (waited_ms >= kRingbufBlockWarnMs) {
                        ESP_LOGW(TAG, "[PLAYBACK_DIAG] ringbuf full, waited %lu ms",
                                 static_cast<unsigned long>(waited_ms));
                    }
                }
                if (((esp_timer_get_time() - feed_start_us) / 1000) >= kFeedBackpressureReturnMs) {
                    break;
                }
                if (backoffMs < kRingbufMaxBackoffMs) {
                    backoffMs *= 2;
                    if (backoffMs > kRingbufMaxBackoffMs) {
                        backoffMs = kRingbufMaxBackoffMs;
                    }
                }
                continue;
            }

            if (free_bytes < toWrite) {
                toWrite = free_bytes;
            }
            backoffMs = 1;

            int written = raw_stream_write(raw, (char *) (buffer + offset), toWrite);
            if (written <= 0) {
                ESP_LOGE(TAG, "[RAW STREAM] Error writing to pipeline: %d", written);
                break;
            }

            offset += static_cast<size_t>(written);
            if (diagnostics_enabled && static_cast<size_t>(written) < toWrite) {
                feedShortWriteCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (diagnostics_enabled) {
            const size_t fill = getBufferFillLevel();
            size_t prev_min = minFillBytes_.load(std::memory_order_relaxed);
            while (fill < prev_min &&
                   !minFillBytes_.compare_exchange_weak(prev_min, fill, std::memory_order_relaxed)) {
            }
            maybeLogPlaybackDiagnostics();
            maybeLogResumeFollowUp();
        }

        return offset;
    }

    size_t ADFAudioSink::getBufferFillLevel() {
        if (!initialized_ || !audioPipeline) {
            return 0;
        }

        if (audioPipeline->i2SStreamReader) {
            return ringbufFilled(
                    audio_element_get_output_ringbuf(audioPipeline->i2SStreamReader.get()));
        }
        return 0;
    }

    size_t ADFAudioSink::getBufferCapacity() {
        if (!initialized_ || !audioPipeline) {
            return 0;
        }

        if (audioPipeline->i2SStreamReader) {
            ringbuf_handle_t rb =
                    audio_element_get_output_ringbuf(audioPipeline->i2SStreamReader.get());
            const int size = rb ? rb_get_size(rb) : 0;
            if (size > 0) {
                return static_cast<size_t>(size);
            }
        }
        return kRawRingBufferSize;
    }

    void ADFAudioSink::setupPipeline() {
        ESP_LOGI(TAG, "Setting up ADF audio pipeline for Spotify");
        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = static_cast<int>(kRawRingBufferSize)
        };

        audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
        if (!raw_handle) {
            ESP_LOGE(TAG, "Failed to create raw stream for Spotify");
            throw std::runtime_error("Failed to create raw stream for Spotify");
        }

        audioPipeline = AudioPipelineUtil::createAudioPipeline(
                AudioSource::SPOTIFY,
                raw_handle
        );

        initialized_ = true;
        ESP_LOGI(TAG, "ADF audio pipeline setup completed");
    }

    void ADFAudioSink::prepare() {
        if (!initialized_) {
            setupPipeline();
        }
    }

    void ADFAudioSink::start() {
        if (!initialized_) {
            setupPipeline();
        }

        if (audioPipeline && !pipelineRunning_) {
            audio_pipeline_run(audioPipeline->pipeline.get());
            pipelineRunning_ = true;
            isPlaying_.store(true);
            ESP_LOGI(TAG, "ADF audio pipeline started");
        } else if (!audioPipeline) {
            ESP_LOGE(TAG, "Audio pipeline not initialized");
        }
    }

    void ADFAudioSink::resume() {
        if (!audioPipeline || !audioPipeline->pipeline) {
            return;
        }

        const size_t fill_before = getBufferFillLevel();
        const size_t capacity = getBufferCapacity();
        const int i2s_state_before = i2sElementState(audioPipeline->i2SStreamWriter);

        const esp_err_t ret = audio_pipeline_resume(audioPipeline->pipeline.get());
        const size_t fill_after = getBufferFillLevel();
        const int i2s_state_after = i2sElementState(audioPipeline->i2SStreamWriter);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
            if (diagnostics_enabled) {
                ESP_LOGW(TAG,
                         "[PLAYBACK_RESUME_DIAG] resume failed ret=%s fill=%u->%u cap=%u "
                         "i2s_state=%d->%d",
                         esp_err_to_name(ret),
                         static_cast<unsigned>(fill_before),
                         static_cast<unsigned>(fill_after),
                         static_cast<unsigned>(capacity),
                         i2s_state_before,
                         i2s_state_after);
            }
            return;
        }

        isPlaying_.store(true);
        ESP_LOGI(TAG, "Pipeline resumed");

        if (diagnostics_enabled) {
            resumeDiagStartUs_ = esp_timer_get_time();
            resumeDiag500Logged_ = false;
            resumeDiag2000Logged_ = false;
            ESP_LOGI(TAG,
                     "[PLAYBACK_RESUME_DIAG] resume ok fill=%u->%u cap=%u i2s_state=%d->%d playing=1",
                     static_cast<unsigned>(fill_before),
                     static_cast<unsigned>(fill_after),
                     static_cast<unsigned>(capacity),
                     i2s_state_before,
                     i2s_state_after);
        }
    }

    void ADFAudioSink::pause() {
        if (audioPipeline && audioPipeline->pipeline && isPlaying_.load()) {
            esp_err_t ret = audio_pipeline_pause(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
            } else {
                isPlaying_.store(false);
                ESP_LOGI(TAG, "Pipeline paused");
            }
        }
    }

    void ADFAudioSink::flush() {
        if (!audioPipeline || !audioPipeline->i2SStreamReader) {
            ESP_LOGW(TAG, "Cannot flush - pipeline or reader not initialized");
            return;
        }

        pause();

        ESP_LOGI(TAG, "Flushing audio pipeline buffers");

        auto raw = audioPipeline->i2SStreamReader.get();
        if (raw) {
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
            rb = audio_element_get_output_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
        }

        if (audioPipeline->i2SStreamWriter) {
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(audioPipeline->i2SStreamWriter.get());
            if (rb) {
                rb_reset(rb);
            }
        }

        ESP_LOGI(TAG, "Audio pipeline buffers flushed");
    }

    void ADFAudioSink::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up ADF audio pipeline");
        if (audioPipeline) {
            AudioPipelineUtil::cleanupPipeline(*audioPipeline);
        }
        initialized_ = false;
        pipelineRunning_ = false;
        ESP_LOGI(TAG, "ADF audio pipeline cleanup complete");
    }


}
