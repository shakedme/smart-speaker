//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_AUDIO_TYPES_H
#define BT_SPEAKER_AUDIO_TYPES_H

#pragma once

#include <cstdint>
#include <cstddef>  // For size_t
#include <array>

extern "C" {
#include "esp_timer.h"
}

namespace smart_speaker {

// Audio sample types
    using Sample16 = int16_t;
    using Sample32 = int32_t;
    using SampleFloat = float;

// System-wide audio configuration
    struct AudioConfig {
        static constexpr uint32_t SAMPLE_RATE = 44100;
        static constexpr uint8_t CHANNELS = 2;
        static constexpr uint16_t FRAME_SIZE = 1024;          // Samples per processing frame
        static constexpr uint8_t FRAME_TIME_MS = 23;          // ~23ms per frame at 44.1kHz
        static constexpr uint8_t DMA_BUFFER_COUNT = 8;        // I2S DMA buffers
        static constexpr uint16_t VOICE_SAMPLE_RATE = 16000;  // Voice processing rate
    };

// Audio source identification
    enum class AudioSource : uint8_t {
        Bluetooth = 0,
        Spotify = 1,
        Voice = 2,
        System = 3,
        Invalid = 255
    };

// Audio processing priorities (higher = more important)
    enum class AudioPriority : uint8_t {
        Background = 50,    // System sounds
        Music = 100,        // Spotify streaming
        Bluetooth = 150,    // BT audio (main use case)
        Voice = 200,        // Voice commands
        Emergency = 255     // Critical system audio
    };

// Audio processing states
    enum class AudioState : uint8_t {
        Idle,
        BluetoothStreaming,
        SpotifyStreaming,
        VoiceListening,
        VoiceResponding,
        Processing,
        Error
    };

// Standard audio frame for processing
    template<size_t FrameSize = AudioConfig::FRAME_SIZE>
    struct AudioFrame {
        std::array<Sample16, FrameSize> samples;
        uint32_t timestamp_us;
        AudioSource source;
        uint8_t channels;
        uint32_t sample_rate;

        // Helper methods
        constexpr size_t size() const { return FrameSize; }
        constexpr size_t bytes() const { return FrameSize * sizeof(Sample16); }
        constexpr float duration_ms() const {
            return (FrameSize * 1000.0f) / sample_rate;
        }

        void clear() {
            samples.fill(0);
            timestamp_us = 0;
            source = AudioSource::Invalid;
        }
    };

// Common audio frame types
    using StandardFrame = AudioFrame<AudioConfig::FRAME_SIZE>;
    using VoiceFrame = AudioFrame<256>;  // Smaller frames for voice processing

// Audio volume control (0.0 to 1.0, with >1.0 for gain)
    struct VolumeControl {
        float volume = 1.0f;
        bool muted = false;

        constexpr float apply(Sample16 sample) const {
            return muted ? 0.0f : sample * volume;
        }

        constexpr bool is_valid() const {
            return volume >= 0.0f && volume <= 2.0f;  // Allow up to 2x gain
        }
    };

// Performance metrics for monitoring
    struct AudioMetrics {
        uint32_t frames_processed = 0;
        uint32_t buffer_underruns = 0;
        uint32_t buffer_overruns = 0;
        uint32_t max_processing_time_us = 0;
        float cpu_load_percent = 0.0f;

        void reset() {
            frames_processed = 0;
            buffer_underruns = 0;
            buffer_overruns = 0;
            max_processing_time_us = 0;
            cpu_load_percent = 0.0f;
        }
    };

// Error types for audio system
    enum class AudioError : int {
        OK = 0,
        BufferFull = -1,
        BufferEmpty = -2,
        InvalidFormat = -3,
        HardwareError = -4,
        TimeoutError = -5,
        ConfigError = -6
    };

} // namespace smart_speaker

#endif //BT_SPEAKER_AUDIO_TYPES_H