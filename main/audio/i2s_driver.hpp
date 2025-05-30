//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_I2S_DRIVER_H
#define BT_SPEAKER_I2S_DRIVER_H

#pragma once

#include "audio_types.hpp"
#include "../shared/event_bus.hpp"
#include <memory>
#include <atomic>

extern "C" {
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
}

namespace smart_speaker {

// I2S hardware configuration for PCM5102A DAC
    struct I2SConfig {
        // GPIO pins
        gpio_num_t bclk_pin = GPIO_NUM_25;    // Bit clock
        gpio_num_t ws_pin = GPIO_NUM_22;      // Word select (LRC)
        gpio_num_t dout_pin = GPIO_NUM_26;    // Data output

        // Audio format
        uint32_t sample_rate = AudioConfig::SAMPLE_RATE;
        uint8_t channels = AudioConfig::CHANNELS;
        i2s_data_bit_width_t bit_width = I2S_DATA_BIT_WIDTH_16BIT;

        // DMA configuration
        uint8_t dma_desc_num = AudioConfig::DMA_BUFFER_COUNT;
        uint16_t dma_frame_num = AudioConfig::FRAME_SIZE / 4;  // Frames per DMA buffer

        // Validation
        bool is_valid() const {
            return sample_rate > 0 && channels > 0 &&
                   bclk_pin != GPIO_NUM_NC && ws_pin != GPIO_NUM_NC && dout_pin != GPIO_NUM_NC;
        }
    };

// Hardware abstraction for I2S audio output (Single Responsibility)
    class I2SDriver {
    private:
        i2s_chan_handle_t tx_handle_;
        I2SConfig config_;
        std::atomic<bool> initialized_;
        std::atomic<bool> running_;

        // Performance metrics
        mutable std::atomic<uint32_t> write_count_;
        mutable std::atomic<uint32_t> error_count_;
        mutable std::atomic<uint32_t> bytes_written_;

        static constexpr const char *TAG = "I2S_DRIVER";

    public:
        explicit I2SDriver(const I2SConfig &config = I2SConfig{});

        ~I2SDriver();

        // Non-copyable, movable
        I2SDriver(const I2SDriver &) = delete;

        I2SDriver &operator=(const I2SDriver &) = delete;

        I2SDriver(I2SDriver &&) = default;

        I2SDriver &operator=(I2SDriver &&) = default;

        // Lifecycle management
        AudioError initialize();

        AudioError start();

        AudioError stop();

        void shutdown();

        // Audio output interface
        AudioError write_frame(const StandardFrame &frame);

        AudioError write_samples(const Sample16 *samples, size_t count);

        AudioError write_raw(const void *data, size_t bytes, size_t *bytes_written = nullptr);

        // Configuration management
        AudioError set_sample_rate(uint32_t sample_rate);

        AudioError set_volume(float volume);  // Hardware volume if supported

        // Status queries
        bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }

        bool is_running() const { return running_.load(std::memory_order_acquire); }

        const I2SConfig &get_config() const { return config_; }

        // Performance metrics
        uint32_t get_write_count() const { return write_count_.load(std::memory_order_relaxed); }

        uint32_t get_error_count() const { return error_count_.load(std::memory_order_relaxed); }

        uint32_t get_bytes_written() const { return bytes_written_.load(std::memory_order_relaxed); }

        void reset_metrics();

        // Hardware diagnostics
        bool test_hardware();

        std::string get_status_string() const;

    private:
        AudioError configure_channel();

        AudioError configure_standard_mode();

        void increment_error_count() const;

        void log_performance_stats() const;
    };

// Factory for creating I2S drivers with common configurations
    class I2SDriverFactory {
    public:
        // Standard PCM5102A configuration
        static std::unique_ptr<I2SDriver> create_pcm5102a_driver() {
            I2SConfig config;
            config.bclk_pin = GPIO_NUM_25;
            config.ws_pin = GPIO_NUM_22;
            config.dout_pin = GPIO_NUM_26;
            config.sample_rate = 44100;
            config.channels = 2;

            return std::make_unique<I2SDriver>(config);
        }

        // High-quality audio configuration
        static std::unique_ptr<I2SDriver> create_hifi_driver() {
            I2SConfig config;
            config.bclk_pin = GPIO_NUM_25;
            config.ws_pin = GPIO_NUM_22;
            config.dout_pin = GPIO_NUM_26;
            config.sample_rate = 48000;  // Higher sample rate
            config.channels = 2;
            config.dma_desc_num = 16;    // More buffers for stability

            return std::make_unique<I2SDriver>(config);
        }

        // Low-latency configuration
        static std::unique_ptr<I2SDriver> create_low_latency_driver() {
            I2SConfig config;
            config.bclk_pin = GPIO_NUM_25;
            config.ws_pin = GPIO_NUM_22;
            config.dout_pin = GPIO_NUM_26;
            config.sample_rate = 44100;
            config.channels = 2;
            config.dma_desc_num = 4;     // Fewer buffers for lower latency
            config.dma_frame_num = 128;  // Smaller DMA buffers

            return std::make_unique<I2SDriver>(config);
        }
    };

} // namespace smart_speaker

#endif //BT_SPEAKER_I2S_DRIVER_H
