#include "../include/microphone_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/memory_utils.hpp"
#include <cmath>

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
        ESP_LOGI(TAG, "MicrophoneManager created for SPH0645LM4H I2S MEMS microphones");
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

    esp_err_t MicrophoneManager::init() {
        ESP_LOGI(TAG, "Initializing microphone manager");

        esp_err_t ret = setupMicrophonePipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup microphone pipeline: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Microphone manager initialized successfully");
        return ESP_OK;
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
        ESP_LOGI(TAG, "Audio processing task started with direct I2S channel reading");

        auto *raw_buffer = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
        auto *stereo_buffer = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_SIZE * 2, MALLOC_CAP_SPIRAM));
        
        if (!raw_buffer || !stereo_buffer) {
            ESP_LOGE(TAG, "Failed to allocate audio buffers");
            if (raw_buffer) free(raw_buffer);
            if (stereo_buffer) free(stereo_buffer);
            return;
        }

        // Wait for microphone to stabilize
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Read and discard initial samples
        for (int i = 0; i < 100; i++) {
            size_t bytes_read;
            i2s_channel_read(i2s_rx_handle_, raw_buffer, AUDIO_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(10));
        }

        ESP_LOGI(TAG, "Microphone stabilized, starting audio processing with VAD pre-filter");

        while (recording_) {
            // Read audio directly from I2S channel
            size_t bytes_read;
            esp_err_t ret = i2s_channel_read(i2s_rx_handle_, raw_buffer, AUDIO_BUFFER_SIZE, &bytes_read, pdMS_TO_TICKS(100));
            
            if (ret == ESP_OK && bytes_read > 0) {
                if (audioCallback_) {
                    // VAD pre-filter re-enabled after switching to WakeNet9s (ESP32-compatible)
                    if (hasVoiceActivity(raw_buffer, bytes_read)) {
                        // Convert mono to stereo and feed to wake word detector
                        size_t stereo_bytes = convertMonoToStereo(raw_buffer, stereo_buffer, bytes_read);
                        audioCallback_(stereo_buffer, stereo_bytes);
                    }
                    // Skip ESP-SR processing during silence - saves CPU and prevents buffer overflow
                }
            }

            vTaskDelay(pdMS_TO_TICKS(5)); // Reduced delay for more responsive audio processing
        }

        free(raw_buffer);
        free(stereo_buffer);
        ESP_LOGI(TAG, "Audio processing task ended");
    }

    esp_err_t MicrophoneManager::setupMicrophonePipeline() {
        ESP_LOGI(TAG, "Setting up direct I2S STD driver for SPH0645LM4H (new API, compatible with ESP-ADF)");

        // Create I2S channel handle (new API)
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = 4;
        chan_cfg.dma_frame_num = 128;  // Same as working test

        esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
            return ret;
        }

        // Configure I2S standard mode (based on working test)
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),  // Back to 44.1kHz like working test
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
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

    size_t MicrophoneManager::convertMonoToStereo(const uint8_t* mono_data, uint8_t* stereo_data, size_t mono_bytes) {
        if (!mono_data || !stereo_data || mono_bytes == 0) {
            return 0;
        }

        // SPH0645 gives us 32-bit samples that need 14-bit right shift
        const auto* raw_samples = reinterpret_cast<const int32_t*>(mono_data);
        auto* stereo_samples = reinterpret_cast<int16_t*>(stereo_data);
        
        size_t raw_sample_count = mono_bytes / sizeof(int32_t);
        const int SHIFT_VALUE = 14;  // SPH0645 specific shift
        
        // Better downsampling: 44100 Hz to 16000 Hz
        // Ratio = 44100/16000 = 2.75625, so we'll use 2.75 (11:4 ratio)
        // This gives us 44100/2.75 = 16036 Hz (very close to 16000 Hz)
        static size_t downsample_counter = 0;
        size_t output_sample_pairs = 0;
        
        // Convert 32-bit SPH0645 samples to 16-bit stereo with precise downsampling
        for (size_t i = 0; i < raw_sample_count; i++) {
            // Apply SPH0645 14-bit shift to get the actual audio data
            int32_t shifted_sample = raw_samples[i] >> SHIFT_VALUE;
            
            // Convert to 16-bit range (shifted_sample is roughly -8000 to +8000)
            // Scale to use more of the 16-bit range for better SNR
            int32_t scaled_sample = shifted_sample * 4;  // Scale up by 4x
            
            // Clamp to 16-bit range
            if (scaled_sample > 32767) scaled_sample = 32767;
            if (scaled_sample < -32768) scaled_sample = -32768;
            
            int16_t final_sample = (int16_t)scaled_sample;
            
            // Downsampling logic: keep 4 out of every 11 samples (approximately 2.75:1)
            // This pattern: keep, keep, skip, keep, skip, keep, skip, skip, skip, skip, skip
            downsample_counter++;
            bool keep_sample = false;
            
            switch (downsample_counter % 11) {
                case 1: case 2: case 4: case 6:  // Keep these positions
                    keep_sample = true;
                    break;
                default:  // Skip others
                    keep_sample = false;
                    break;
            }
            
            if (keep_sample) {
                // Create stereo by duplicating the mono channel
                stereo_samples[output_sample_pairs * 2] = final_sample;     // Left channel
                stereo_samples[output_sample_pairs * 2 + 1] = final_sample; // Right channel (duplicate)
                output_sample_pairs++;
            }
        }
        
        // Return stereo byte count (16-bit samples, 2 channels, downsampled)
        return output_sample_pairs * 2 * sizeof(int16_t);
    }

    bool MicrophoneManager::hasVoiceActivity(const uint8_t* audio_buffer, size_t bytes_read) {
        if (!audio_buffer || bytes_read == 0) {
            return false;
        }

        // Handle 32-bit samples from SPH0645 and apply proper shift
        const auto* raw_samples = reinterpret_cast<const int32_t*>(audio_buffer);
        size_t raw_sample_count = bytes_read / sizeof(int32_t);
        
        if (raw_sample_count < 10) {  // Need minimum samples for analysis
            return false;
        }

        const int SHIFT_VALUE = 14;  // SPH0645 specific shift
        
        // SIMPLIFIED VAD: Much more permissive, only block obvious issues
        int64_t dc_sum = 0;
        int64_t energy_sum = 0;
        bool has_non_zero = false;
        
        for (size_t i = 0; i < raw_sample_count; i++) {
            int32_t shifted_sample = raw_samples[i] >> SHIFT_VALUE;
            dc_sum += shifted_sample;
            energy_sum += (int64_t)shifted_sample * shifted_sample;
            
            if (shifted_sample != 0) {
                has_non_zero = true;
            }
        }
        
        // Check 1: Ensure we have non-zero data (not a completely silent buffer)
        if (!has_non_zero) {
            static int silent_count = 0;
            silent_count++;
            if (silent_count % 100 == 0) {
                ESP_LOGD(TAG, "VAD: Blocking silent buffer (count=%d)", silent_count);
            }
            return false;
        }
        
        // Check 2: Basic DC bias sanity check (very wide range)
        double dc_bias = (double)dc_sum / raw_sample_count;
        if (dc_bias > -1000 || dc_bias < -15000) {
            static int bias_fail_count = 0;
            bias_fail_count++;
            if (bias_fail_count % 100 == 0) {
                ESP_LOGD(TAG, "VAD: Unusual DC bias %.1f (count=%d)", dc_bias, bias_fail_count);
            }
            // Still allow through - just log the issue
        }
        
        // Check 3: Very low energy threshold (much more permissive)
        double total_rms = sqrt((double)energy_sum / raw_sample_count);
        const double MIN_ENERGY_THRESHOLD = 5.0;  // Very low threshold
        
        if (total_rms < MIN_ENERGY_THRESHOLD) {
            static int low_energy_count = 0;
            low_energy_count++;
            if (low_energy_count % 200 == 0) {
                ESP_LOGD(TAG, "VAD: Very low energy %.1f (count=%d)", total_rms, low_energy_count);
            }
            return false;  // Only block extremely quiet audio
        }
        
        // Debug logging (very infrequent)
        static int pass_count = 0;
        pass_count++;
        if (pass_count % 500 == 0) {
            ESP_LOGD(TAG, "VAD: Passing audio - DC=%.1f, RMS=%.1f (pass_count=%d)", 
                     dc_bias, total_rms, pass_count);
        }
        
        // MUCH MORE PERMISSIVE: Allow almost all audio through
        return true;
    }

} // namespace btspeaker 