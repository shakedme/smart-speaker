//
// Created by Shaked Melman on 29/05/2025.
//

//
// I2S Driver Implementation - Add to audio/i2s_driver.cpp
//

#include "i2s_driver.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
}

namespace smart_speaker {

// Constructor
    I2SDriver::I2SDriver(const I2SConfig& config)
            : tx_handle_(nullptr),
              config_(config),
              initialized_(false),
              running_(false),
              write_count_(0),
              error_count_(0),
              bytes_written_(0) {
        
        if (!config_.is_valid()) {
            ESP_LOGE(TAG, "Invalid I2S configuration");
            return;
        }
        
        ESP_LOGI(TAG, "I2SDriver constructor - GPIO pins: BCLK=%d, WS=%d, DOUT=%d", 
                 config_.bclk_pin, config_.ws_pin, config_.dout_pin);
    }

// Destructor
    I2SDriver::~I2SDriver() {
        ESP_LOGI(TAG, "I2SDriver destructor");
        shutdown();
    }

// Initialize I2S hardware
    AudioError I2SDriver::initialize() {
        ESP_LOGI(TAG, "Initializing I2S driver...");
        
        if (initialized_.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "I2S already initialized");
            return AudioError::OK;
        }

        // Configure I2S channel
        AudioError result = configure_channel();
        if (result != AudioError::OK) {
            ESP_LOGE(TAG, "Failed to configure I2S channel");
            return result;
        }

        // Configure standard mode
        result = configure_standard_mode();
        if (result != AudioError::OK) {
            ESP_LOGE(TAG, "Failed to configure I2S standard mode");
            return result;
        }

        initialized_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "I2S driver initialized successfully");
        return AudioError::OK;
    }

// Start I2S transmission
    AudioError I2SDriver::start() {
        ESP_LOGI(TAG, "Starting I2S transmission...");
        
        if (!initialized_.load(std::memory_order_acquire)) {
            ESP_LOGE(TAG, "I2S not initialized");
            return AudioError::ConfigError;
        }
        
        if (running_.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "I2S already running");
            return AudioError::OK;
        }

        // Enable I2S channel
        esp_err_t ret = i2s_channel_enable(tx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
            increment_error_count();
            return AudioError::HardwareError;
        }

        running_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "I2S transmission started");
        return AudioError::OK;
    }

// Stop I2S transmission
    AudioError I2SDriver::stop() {
        ESP_LOGI(TAG, "Stopping I2S transmission...");
        
        if (!running_.load(std::memory_order_acquire)) {
            return AudioError::OK;
        }

        // Disable I2S channel
        esp_err_t ret = i2s_channel_disable(tx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
        }

        running_.store(false, std::memory_order_release);
        ESP_LOGI(TAG, "I2S transmission stopped");
        return AudioError::OK;
    }

// Shutdown I2S driver
    void I2SDriver::shutdown() {
        ESP_LOGI(TAG, "Shutting down I2S driver...");
        
        stop();
        
        if (tx_handle_) {
            esp_err_t ret = i2s_del_channel(tx_handle_);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(ret));
            }
            tx_handle_ = nullptr;
        }
        
        initialized_.store(false, std::memory_order_release);
        ESP_LOGI(TAG, "I2S driver shutdown complete");
    }

// Write audio frame
    AudioError I2SDriver::write_frame(const StandardFrame& frame) {
        if (!running_.load(std::memory_order_acquire)) {
            return AudioError::ConfigError;
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(tx_handle_, 
                                          frame.samples.data(), 
                                          frame.bytes(), 
                                          &bytes_written, 
                                          portMAX_DELAY);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            increment_error_count();
            return AudioError::HardwareError;
        }

        if (bytes_written != frame.bytes()) {
            ESP_LOGW(TAG, "Incomplete I2S write: %zu/%zu bytes", bytes_written, frame.bytes());
            increment_error_count();
            return AudioError::BufferFull;
        }

        write_count_.fetch_add(1, std::memory_order_relaxed);
        bytes_written_.fetch_add(bytes_written, std::memory_order_relaxed);

        return AudioError::OK;
    }

// Write samples directly
    AudioError I2SDriver::write_samples(const Sample16* samples, size_t count) {
        if (!running_.load(std::memory_order_acquire)) {
            return AudioError::ConfigError;
        }

        size_t bytes_to_write = count * sizeof(Sample16);
        size_t bytes_written = 0;
        
        esp_err_t ret = i2s_channel_write(tx_handle_, 
                                          samples, 
                                          bytes_to_write, 
                                          &bytes_written, 
                                          portMAX_DELAY);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write samples failed: %s", esp_err_to_name(ret));
            increment_error_count();
            return AudioError::HardwareError;
        }

        bytes_written_.fetch_add(bytes_written, std::memory_order_relaxed);
        return AudioError::OK;
    }

// Write raw data
    AudioError I2SDriver::write_raw(const void* data, size_t bytes, size_t* bytes_written) {
        if (!running_.load(std::memory_order_acquire)) {
            return AudioError::ConfigError;
        }

        size_t actual_bytes_written = 0;
        esp_err_t ret = i2s_channel_write(tx_handle_, 
                                          data, 
                                          bytes, 
                                          &actual_bytes_written, 
                                          portMAX_DELAY);
        
        if (bytes_written) {
            *bytes_written = actual_bytes_written;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write raw failed: %s", esp_err_to_name(ret));
            increment_error_count();
            return AudioError::HardwareError;
        }

        bytes_written_.fetch_add(actual_bytes_written, std::memory_order_relaxed);
        return AudioError::OK;
    }

// Set sample rate (requires reconfiguration)
    AudioError I2SDriver::set_sample_rate(uint32_t sample_rate) {
        ESP_LOGI(TAG, "Setting I2S sample rate to %lu Hz", sample_rate);
        
        if (sample_rate == config_.sample_rate) {
            return AudioError::OK;
        }

        bool was_running = running_.load(std::memory_order_acquire);
        
        // Stop if running
        if (was_running) {
            stop();
        }

        config_.sample_rate = sample_rate;

        // Reconfigure if initialized
        if (initialized_.load(std::memory_order_acquire)) {
            AudioError result = configure_standard_mode();
            if (result != AudioError::OK) {
                ESP_LOGE(TAG, "Failed to reconfigure I2S with new sample rate");
                return result;
            }
        }

        // Restart if it was running
        if (was_running) {
            return start();
        }

        return AudioError::OK;
    }

// Set volume (software scaling)
    AudioError I2SDriver::set_volume(float volume) {
        ESP_LOGI(TAG, "I2S volume control not implemented (use external amplifier)");
        return AudioError::OK;
    }

// Reset performance metrics
    void I2SDriver::reset_metrics() {
        write_count_.store(0, std::memory_order_relaxed);
        error_count_.store(0, std::memory_order_relaxed);
        bytes_written_.store(0, std::memory_order_relaxed);
        ESP_LOGI(TAG, "I2S metrics reset");
    }

// Test hardware connectivity
    bool I2SDriver::test_hardware() {
        ESP_LOGI(TAG, "Testing I2S hardware...");
        
        if (!initialized_.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "I2S not initialized for hardware test");
            return false;
        }

        // Write a test pattern (silence)
        const size_t test_samples = 1024;
        Sample16 silence[test_samples] = {0};
        
        AudioError result = write_samples(silence, test_samples);
        if (result != AudioError::OK) {
            ESP_LOGE(TAG, "I2S hardware test failed");
            return false;
        }

        ESP_LOGI(TAG, "I2S hardware test passed");
        return true;
    }

// Get status string
    std::string I2SDriver::get_status_string() const {
        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "I2S Driver: Init=%s, Run=%s, Rate=%luHz, Writes=%lu, Errors=%lu",
                 initialized_.load() ? "Y" : "N",
                 running_.load() ? "Y" : "N",
                 config_.sample_rate,
                 write_count_.load(),
                 error_count_.load());
        return std::string(buffer);
    }

// Configure I2S channel
    AudioError I2SDriver::configure_channel() {
        ESP_LOGI(TAG, "Configuring I2S channel...");

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;  // Clear DMA buffer on underrun
        chan_cfg.dma_desc_num = config_.dma_desc_num;
        chan_cfg.dma_frame_num = config_.dma_frame_num;

        esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
            return AudioError::HardwareError;
        }

        ESP_LOGI(TAG, "I2S channel configured successfully");
        return AudioError::OK;
    }

// Configure I2S standard mode for PCM5102A
    AudioError I2SDriver::configure_standard_mode() {
        ESP_LOGI(TAG, "Configuring I2S standard mode for PCM5102A...");

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(config_.bit_width, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,  // PCM5102A has internal PLL
                .bclk = config_.bclk_pin,
                .ws = config_.ws_pin,
                .dout = config_.dout_pin,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        esp_err_t ret = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
            return AudioError::HardwareError;
        }

        ESP_LOGI(TAG, "I2S standard mode configured: %luHz, %d-bit, stereo", 
                 config_.sample_rate, config_.bit_width);
        return AudioError::OK;
    }

// Increment error counter (thread-safe)
    void I2SDriver::increment_error_count() const {
        error_count_.fetch_add(1, std::memory_order_relaxed);
    }

// Log performance statistics
    void I2SDriver::log_performance_stats() const {
        ESP_LOGI(TAG, "=== I2S Performance Stats ===");
        ESP_LOGI(TAG, "Writes: %lu", write_count_.load());
        ESP_LOGI(TAG, "Errors: %lu", error_count_.load());
        ESP_LOGI(TAG, "Bytes written: %lu", bytes_written_.load());
        ESP_LOGI(TAG, "Sample rate: %lu Hz", config_.sample_rate);
        ESP_LOGI(TAG, "GPIO - BCLK: %d, WS: %d, DOUT: %d", 
                 config_.bclk_pin, config_.ws_pin, config_.dout_pin);
    }

} // namespace smart_speaker