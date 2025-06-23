#include "../include/microphone_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/memory_utils.hpp"
#include <cmath>
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "board.h"
#include "i2s_stream.h"
#include "audio_common.h"
#include "soc/i2s_reg.h"  // Add this for SPH0645 timing fixes
#include "driver/i2s_std.h"   // New I2S standard driver (compatible with ESP-ADF)
}

namespace btspeaker {

    MicrophoneManager::MicrophoneManager()
            : recording_(false), audioTaskHandle_(nullptr) {
        ESP_LOGI(TAG, "MicrophoneManager created for SPH0645LM4H I2S MEMS microphone (mono output)");
    }

    MicrophoneManager::~MicrophoneManager() {
        shutdown();
    }

    void MicrophoneManager::applySPH0645Fix(i2s_port_t i2s_num) {
        // Critical timing adjustments for SPH0645LM4H - same as working test
        REG_SET_BIT(I2S_TIMING_REG(i2s_num), BIT(9));        // I2S_RX_SD_IN_DELAY
        REG_SET_BIT(I2S_CONF_REG(i2s_num), I2S_RX_MSB_SHIFT); // Phillips I2S - WS changes a cycle earlier

        ESP_LOGI(TAG, "Applied SPH0645LM4H timing fixes to I2S%d", i2s_num);
    }

    void MicrophoneManager::init() {
        ESP_LOGI(TAG, "Initializing microphone manager");

        esp_err_t ret = setupMicrophonePipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup microphone pipeline: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize microphone manager");
        }

        ESP_LOGI(TAG, "Microphone manager initialized successfully");
    }

    esp_err_t MicrophoneManager::startRecording() {
        if (recording_) {
            ESP_LOGW(TAG, "Already recording");
            return ESP_OK;
        }

        // Log memory stats before task creation
        MemoryUtils::logMemoryStats("Before audio task creation");

        recording_ = true;

        // Create audio processing task with PSRAM stack
        BaseType_t task_created = xTaskCreatePinnedToCore(
                audioTask,
                "mic_audio",
                3072, // Reduced stack size from 4096 to save memory (~1KB saved)
                this,
                5,    // Priority
                &audioTaskHandle_,
                1     // Pin to core 1 (app core), may help with memory allocation
        );

        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio task");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Microphone recording started");
        return ESP_OK;
    }

    esp_err_t MicrophoneManager::stopRecording() {
        if (!recording_) {
            return ESP_OK;
        }

        recording_ = false;

        if (audioTaskHandle_) {
            vTaskDelete(audioTaskHandle_);
            audioTaskHandle_ = nullptr;
        }

        ESP_LOGI(TAG, "Microphone recording stopped");
        return ESP_OK;
    }

    void MicrophoneManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down microphone manager");

        stopRecording();
        cleanupPipeline();
    }

    void MicrophoneManager::setAudioCallback(AudioDataCallback callback) {
        audioCallback_ = std::move(callback);
    }

    void MicrophoneManager::audioTask(void *arg) {
        auto *self = static_cast<MicrophoneManager *>(arg);
        self->runAudioProcessing();
        vTaskDelete(NULL);
    }

    void MicrophoneManager::runAudioProcessing() {
        ESP_LOGI(TAG, "Audio processing task started with direct I2S channel reading (mono output)");

        auto *raw_buffer = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
        auto *processed_buffer = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM));

        if (!raw_buffer || !processed_buffer) {
            ESP_LOGE(TAG, "Failed to allocate audio buffers");
            if (raw_buffer) free(raw_buffer);
            if (processed_buffer) free(processed_buffer);
            return;
        }

        // Wait for microphone to stabilize
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Read and discard initial samples
        for (int i = 0; i < 100; i++) {
            size_t bytes_read;
            i2s_channel_read(i2s_rx_handle_, raw_buffer, AUDIO_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(10));
        }

        ESP_LOGI(TAG, "Microphone stabilized, starting mono audio processing with VAD pre-filter");

        while (recording_) {
            // Read audio directly from I2S channel
            size_t bytes_read;
            esp_err_t ret = i2s_channel_read(i2s_rx_handle_, raw_buffer, AUDIO_BUFFER_SIZE, &bytes_read,
                                             portMAX_DELAY);

            if (ret == ESP_OK && bytes_read > 0) {
                if (audioCallback_) {
                    size_t processed_bytes = processMonoAudio(raw_buffer, processed_buffer, bytes_read);
                    if (processed_bytes > 0) {
                        audioCallback_(processed_buffer, processed_bytes);
                    }
                    // Backend will handle VAD and wake word detection
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1)); // Minimal delay for maximum audio responsiveness
        }

        free(raw_buffer);
        free(processed_buffer);
        ESP_LOGI(TAG, "Audio processing task ended");
    }

    esp_err_t MicrophoneManager::setupMicrophonePipeline() {
        ESP_LOGI(TAG, "Setting up direct I2S STD driver for SPH0645LM4H (new API, compatible with ESP-ADF)");

        // Create I2S channel handle (new API) - optimized for audio quality
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = 6;     // More DMA descriptors for smoother audio
        chan_cfg.dma_frame_num = 256;  // Larger DMA frames for better audio quality

        esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
            return ret;
        }

        // Configure I2S standard mode (16kHz for direct VAD/wake word compatibility)
        // Using Philips I2S mode as specified in official Adafruit SPH0645 example
        i2s_std_config_t std_cfg = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),  // 16kHz - Direct compatibility with VAD/wake word
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                        .mclk = GPIO_NUM_NC,                    // MCLK not needed for SPH0645LM4H
                        .bclk = CONFIG_MIC_I2S_BCK_GPIO,        // GPIO32 - Bit Clock
                        .ws = CONFIG_MIC_I2S_WS_GPIO,           // GPIO33 - Word Select (L/R)
                        .dout = GPIO_NUM_NC,                    // Not used for input
                        .din = CONFIG_MIC_I2S_DIN_GPIO,         // GPIO34 - Data Input
                        .invert_flags = {
                                .mclk_inv = false,
                                .bclk_inv = false,
                                .ws_inv = false,
                        },
                },
        };

        // Initialize I2S channel
        ESP_LOGI(TAG, "Initializing I2S STD channel...");
        ret = i2s_channel_init_std_mode(i2s_rx_handle_, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2S STD channel: %s", esp_err_to_name(ret));
            i2s_del_channel(i2s_rx_handle_);
            return ret;
        }

        // Apply SPH0645 timing fixes (critical! Must be after channel init)
        applySPH0645Fix(I2S_NUM_1);

        // Enable I2S channel
        ESP_LOGI(TAG, "Enabling I2S channel...");
        ret = i2s_channel_enable(i2s_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
            i2s_del_channel(i2s_rx_handle_);
            return ret;
        }

        ESP_LOGI(TAG, "Direct I2S STD driver initialized successfully for SPH0645!");
        return ESP_OK;
    }

    esp_err_t MicrophoneManager::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up direct I2S driver");

        if (i2s_rx_handle_) {
            i2s_channel_disable(i2s_rx_handle_);
            i2s_del_channel(i2s_rx_handle_);
            i2s_rx_handle_ = nullptr;
        }

        return ESP_OK;
    }

    size_t MicrophoneManager::processMonoAudio(const uint8_t *raw_data, uint8_t *processed_data, size_t raw_bytes) {
        if (!raw_data || !processed_data || raw_bytes == 0) {
            return 0;
        }

        const auto *raw_samples = reinterpret_cast<const int32_t *>(raw_data);
        auto *output_samples = reinterpret_cast<int16_t *>(processed_data);

        size_t raw_sample_count = raw_bytes / sizeof(int32_t);

        for (size_t i = 0; i < raw_sample_count; i++) {
            int32_t sample = raw_samples[i];
            int32_t shifted_sample = sample >> 14;
            int32_t scaled_sample = (shifted_sample >> 2) * 2;

            if (scaled_sample > 32767) scaled_sample = 32767;
            if (scaled_sample < -32768) scaled_sample = -32768;

            output_samples[i] = (int16_t) scaled_sample;
        }

        // Return mono byte count (16-bit samples, 1 channel, 44.1kHz)
        return raw_sample_count * sizeof(int16_t);
    }

} // namespace btspeaker 