//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_SHARED_TYPES_H
#define BT_SPEAKER_SHARED_TYPES_H
#pragma once

#include "../audio/audio_types.hpp"
#include <atomic>
#include <array>
#include <memory>
#include "esp_attr.h"

namespace smart_speaker {

// Lock-free circular buffer for real-time audio (DRY principle - single implementation)
    template<typename T, size_t Size>
    class alignas(32) LockFreeRingBuffer {
        static_assert((Size & (Size - 1)) == 0, "Size must be power of 2 for efficient modulo");
        static_assert(Size >= 4, "Minimum buffer size is 4 elements");

    public:
        LockFreeRingBuffer() = default;

    private:
        std::array<T, Size> buffer_;
        std::atomic<size_t> write_index_{0};
        std::atomic<size_t> read_index_{0};

        // Performance tracking
        mutable std::atomic<uint32_t> overrun_count_{0};
        mutable std::atomic<uint32_t> underrun_count_{0};

    public:
        // Producer interface (write side)
        IRAM_ATTR T *reserve_write(size_t count = 1) noexcept {
            const size_t current_write = write_index_.load(std::memory_order_relaxed);
            const size_t next_write = (current_write + count) & (Size - 1);
            const size_t current_read = read_index_.load(std::memory_order_acquire);

            // Check for buffer overflow
            if (((next_write - current_read) & (Size - 1)) >= Size - 1) {
                overrun_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }

            return &buffer_[current_write];
        }

        IRAM_ATTR void commit_write(size_t count = 1) noexcept {
            write_index_.fetch_add(count, std::memory_order_release);
        }

// Convenience method for single item write
        IRAM_ATTR bool write(const T &item) noexcept {
            T *write_ptr = reserve_write(1);
            if (!write_ptr) return false;

            *write_ptr = item;
            commit_write(1);
            return true;
        }

// Consumer interface (read side)
        IRAM_ATTR const T *peek_read(size_t count = 1) noexcept {
            const size_t current_read = read_index_.load(std::memory_order_relaxed);
            const size_t current_write = write_index_.load(std::memory_order_acquire);

// Check for buffer underrun
            if (((current_write - current_read) & (Size - 1)) < count) {
                underrun_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }

            return &buffer_[current_read];
        }

        IRAM_ATTR void commit_read(size_t count = 1) noexcept {
            read_index_.fetch_add(count, std::memory_order_release);
        }

// Convenience method for single item read
        IRAM_ATTR bool read(T &item) noexcept {
            const T *read_ptr = peek_read(1);
            if (!read_ptr) return false;

            item = *read_ptr;
            commit_read(1);
            return true;
        }

// Buffer status (non-real-time safe)
        size_t available_write() const noexcept {
            const size_t write = write_index_.load(std::memory_order_relaxed);
            const size_t read = read_index_.load(std::memory_order_relaxed);
            return (Size - 1) - ((write - read) & (Size - 1));
        }

        size_t available_read() const noexcept {
            const size_t write = write_index_.load(std::memory_order_relaxed);
            const size_t read = read_index_.load(std::memory_order_relaxed);
            return (write - read) & (Size - 1);
        }

        bool empty() const noexcept {
            return available_read() == 0;
        }

        bool full() const noexcept {
            return available_write() == 0;
        }

        float fill_ratio() const noexcept {
            return static_cast<float>(available_read()) / Size;
        }

// Performance metrics
        uint32_t overrun_count() const noexcept {
            return overrun_count_.load(std::memory_order_relaxed);
        }

        uint32_t underrun_count() const noexcept {
            return underrun_count_.load(std::memory_order_relaxed);
        }

        void reset_metrics() noexcept {
            overrun_count_.store(0, std::memory_order_relaxed);
            underrun_count_.store(0, std::memory_order_relaxed);
        }

// Buffer management
        void clear() noexcept {
            write_index_.store(0, std::memory_order_relaxed);
            read_index_.store(0, std::memory_order_relaxed);
            reset_metrics();
        }

        constexpr size_t capacity() const noexcept { return Size; }
    };

// Specialized audio buffers (commonly used types)
    using AudioFrameBuffer = LockFreeRingBuffer<StandardFrame, 8>;      // ~184ms of audio
    using SampleBuffer = LockFreeRingBuffer<Sample16, 4096>;            // Raw sample buffer
    using VoiceFrameBuffer = LockFreeRingBuffer<VoiceFrame, 16>;        // Voice processing

    class AudioBufferFactory {
    public:
        // Create buffer optimized for specific use case
        template<typename T, size_t Size>
        static std::unique_ptr<LockFreeRingBuffer<T, Size>> create_buffer() {
            return std::make_unique<LockFreeRingBuffer<T, Size>>();
        }

        // Standard factory methods using type aliases
        static std::unique_ptr<AudioFrameBuffer> create_audio_frame_buffer() {
            return std::make_unique<AudioFrameBuffer>();
        }

        static std::unique_ptr<SampleBuffer> create_sample_buffer() {
            return std::make_unique<SampleBuffer>();
        }

        static std::unique_ptr<VoiceFrameBuffer> create_voice_buffer() {
            return std::make_unique<VoiceFrameBuffer>();
        }
    };
} // namespace smart_speaker
#endif //BT_SPEAKER_SHARED_TYPES_H
