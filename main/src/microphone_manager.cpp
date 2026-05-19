#include "../include/microphone_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/memory_utils.hpp"
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include "esp_dsp.h"

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "board.h"
#include "i2s_stream.h"
#include "audio_common.h"
#include "dsps_biquad_gen.h"
#include "soc/i2s_reg.h"  // Add this for SPH0645 timing fixes
#include "driver/i2s_std.h"   // New I2S standard driver (compatible with ESP-ADF)
}

namespace btspeaker {

    MicrophoneManager::MicrophoneManager()
            : recording_(false), audioTaskHandle_(nullptr) {
        ESP_LOGI(TAG, "MicrophoneManager created for SPH0645LM4H I2S MEMS microphone (mono output)");
        dsps_biquad_gen_hpf_f32(coeffs, F_NORM, Q_FACTOR);
        floatBuffer = static_cast<float *>(heap_caps_malloc(SAMPLE_COUNT * sizeof(float),
                                                            MALLOC_CAP_SPIRAM));
    }

    MicrophoneManager::~MicrophoneManager() {
        shutdown();
    }

    void MicrophoneManager::applySPH0645Fix(i2s_port_t i2s_num) {
        REG_SET_BIT(I2S_TIMING_REG(i2s_num), BIT(9));
        REG_SET_BIT(I2S_CONF_REG(i2s_num), I2S_RX_MSB_SHIFT);
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

        recording_ = true;

        const size_t stackSizeWords = 1024 * 4;  // 32KB stack
        taskStack = (StackType_t *) heap_caps_malloc(
                stackSizeWords * sizeof(StackType_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

        taskBuffer = (StaticTask_t *) heap_caps_malloc(
                sizeof(StaticTask_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

        // Create audio processing task with PSRAM stack
        audioTaskHandle_ = xTaskCreateStaticPinnedToCore(
                audioTask,
                "mic_audio",
                stackSizeWords,
                this,
                5,    // Priority
                taskStack,
                taskBuffer,
                1     // Pin to core 1 (app core), may help with memory allocation
        );

        if (audioTaskHandle_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create microphone audio task");
            heap_caps_free(taskStack);
            heap_caps_free(taskBuffer);
            taskStack = nullptr;
            taskBuffer = nullptr;
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
        vTaskDelete(nullptr);
    }

    void MicrophoneManager::logMicDiagnostics(const int64_t now_us,
                                              const uint32_t loop_count,
                                              const uint32_t callback_count,
                                              const uint64_t total_bytes_read,
                                              const uint64_t total_bytes_out,
                                              const uint32_t i2s_errors,
                                              const uint32_t zero_reads,
                                              const uint64_t sum_i2s_read_us,
                                              const uint32_t max_i2s_read_us,
                                              const uint64_t sum_process_us,
                                              const uint32_t max_process_us,
                                              const uint64_t sum_callback_us,
                                              const uint32_t max_callback_us) const {
        const uint32_t avg_i2s_read_us = loop_count > 0
                                         ? static_cast<uint32_t>(sum_i2s_read_us / loop_count)
                                         : 0;
        const uint32_t avg_process_us = loop_count > 0
                                        ? static_cast<uint32_t>(sum_process_us / loop_count)
                                        : 0;
        const uint32_t avg_callback_us = callback_count > 0
                                         ? static_cast<uint32_t>(sum_callback_us / callback_count)
                                         : 0;
        const uint32_t effective_hz = (MIC_DIAG_INTERVAL_MS > 0 && total_bytes_out > 0)
                                      ? static_cast<uint32_t>(
                                              (total_bytes_out * 1000ULL) /
                                              (static_cast<uint64_t>(MIC_DIAG_INTERVAL_MS) * 2ULL))
                                      : 0;

        ESP_LOGI(TAG,
                 "[MIC_DIAG] loops=%lu cb=%lu in_bytes=%lu out_bytes=%lu ~%luHz@16k/mono "
                 "i2s_us(avg=%lu max=%lu) proc_us(avg=%lu max=%lu) cb_us(avg=%lu max=%lu) "
                 "i2s_err=%lu zero_rd=%lu dram=%lu",
                 static_cast<unsigned long>(loop_count),
                 static_cast<unsigned long>(callback_count),
                 static_cast<unsigned long>(total_bytes_read),
                 static_cast<unsigned long>(total_bytes_out),
                 static_cast<unsigned long>(effective_hz),
                 static_cast<unsigned long>(avg_i2s_read_us),
                 static_cast<unsigned long>(max_i2s_read_us),
                 static_cast<unsigned long>(avg_process_us),
                 static_cast<unsigned long>(max_process_us),
                 static_cast<unsigned long>(avg_callback_us),
                 static_cast<unsigned long>(max_callback_us),
                 static_cast<unsigned long>(i2s_errors),
                 static_cast<unsigned long>(zero_reads),
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        (void) now_us;
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

        int64_t diag_window_start_us = esp_timer_get_time();
        uint32_t diag_loops = 0;
        uint32_t diag_callbacks = 0;
        uint64_t diag_bytes_read = 0;
        uint64_t diag_bytes_out = 0;
        uint32_t diag_i2s_errors = 0;
        uint32_t diag_zero_reads = 0;
        uint64_t diag_sum_i2s_read_us = 0;
        uint32_t diag_max_i2s_read_us = 0;
        uint64_t diag_sum_process_us = 0;
        uint32_t diag_max_process_us = 0;
        uint64_t diag_sum_callback_us = 0;
        uint32_t diag_max_callback_us = 0;

        while (recording_) {
            const int64_t loop_start_us = esp_timer_get_time();

            size_t bytes_read = 0;
            const esp_err_t ret = i2s_channel_read(i2s_rx_handle_, raw_buffer, AUDIO_BUFFER_SIZE, &bytes_read,
                                                   portMAX_DELAY);
            const int64_t after_read_us = esp_timer_get_time();
            const uint32_t i2s_read_us = static_cast<uint32_t>(after_read_us - loop_start_us);

            diag_loops++;
            diag_sum_i2s_read_us += i2s_read_us;
            if (i2s_read_us > diag_max_i2s_read_us) {
                diag_max_i2s_read_us = i2s_read_us;
            }

            if (ret != ESP_OK) {
                diag_i2s_errors++;
                ESP_LOGW(TAG, "[MIC_DIAG] i2s_channel_read failed: %s", esp_err_to_name(ret));
            } else if (bytes_read == 0) {
                diag_zero_reads++;
            } else {
                diag_bytes_read += bytes_read;

                if (audioCallback_) {
                    const int64_t before_process_us = after_read_us;
                    const size_t processed_bytes = processMonoAudio(raw_buffer, processed_buffer, bytes_read);
                    const int64_t after_process_us = esp_timer_get_time();
                    const uint32_t process_us = static_cast<uint32_t>(after_process_us - before_process_us);

                    diag_sum_process_us += process_us;
                    if (process_us > diag_max_process_us) {
                        diag_max_process_us = process_us;
                    }

                    if (processed_bytes > 0) {
                        const int64_t before_callback_us = after_process_us;
                        audioCallback_(processed_buffer, processed_bytes);
                        const int64_t after_callback_us = esp_timer_get_time();
                        const uint32_t callback_us = static_cast<uint32_t>(after_callback_us - before_callback_us);

                        diag_callbacks++;
                        diag_bytes_out += processed_bytes;
                        diag_sum_callback_us += callback_us;
                        if (callback_us > diag_max_callback_us) {
                            diag_max_callback_us = callback_us;
                        }

                        if (callback_us > 50000) {
                            ESP_LOGW(TAG, "[MIC_DIAG] slow upstream callback %lu us for %u bytes",
                                     static_cast<unsigned long>(callback_us),
                                     static_cast<unsigned>(processed_bytes));
                        }
                    }
                }
            }

            if (i2s_read_us > 200000) {
                ESP_LOGW(TAG, "[MIC_DIAG] slow i2s read %lu us, bytes=%u",
                         static_cast<unsigned long>(i2s_read_us),
                         static_cast<unsigned>(bytes_read));
            }

            const int64_t now_us = esp_timer_get_time();
            if ((now_us - diag_window_start_us) >= (MIC_DIAG_INTERVAL_MS * 1000LL)) {
                logMicDiagnostics(now_us,
                                  diag_loops,
                                  diag_callbacks,
                                  diag_bytes_read,
                                  diag_bytes_out,
                                  diag_i2s_errors,
                                  diag_zero_reads,
                                  diag_sum_i2s_read_us,
                                  diag_max_i2s_read_us,
                                  diag_sum_process_us,
                                  diag_max_process_us,
                                  diag_sum_callback_us,
                                  diag_max_callback_us);
                diag_window_start_us = now_us;
                diag_loops = 0;
                diag_callbacks = 0;
                diag_bytes_read = 0;
                diag_bytes_out = 0;
                diag_i2s_errors = 0;
                diag_zero_reads = 0;
                diag_sum_i2s_read_us = 0;
                diag_max_i2s_read_us = 0;
                diag_sum_process_us = 0;
                diag_max_process_us = 0;
                diag_sum_callback_us = 0;
                diag_max_callback_us = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(1));
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
        ret = i2s_channel_init_std_mode(i2s_rx_handle_, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2S STD channel: %s", esp_err_to_name(ret));
            i2s_del_channel(i2s_rx_handle_);
            return ret;
        }

        // Apply SPH0645 timing fixes (critical! Must be after channel init)
        applySPH0645Fix(I2S_NUM_1);

        // Enable I2S channel
        ret = i2s_channel_enable(i2s_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
            i2s_del_channel(i2s_rx_handle_);
            return ret;
        }
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

        // Convert int32 to float and normalize
        for (size_t i = 0; i < SAMPLE_COUNT; i++) {
            int32_t sample = raw_samples[i];
            // Convert to float normalized to [-1, 1] range
            floatBuffer[i] = (float) (sample >> 14) / 32768.0f;
        }

        esp_err_t ret = dsps_biquad_f32_ae32(floatBuffer,
                                             floatBuffer,
                                             static_cast<int>(SAMPLE_COUNT),
                                             coeffs,
                                             delay_lines);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "IIR filter error: %s", esp_err_to_name(ret));
        }

        float maxInt = 32767.0f; // Maximum value for int16_t
        float minInt = -32768.0f; // Minimum value for int16_t

        // Convert back to int16
        for (size_t i = 0; i < SAMPLE_COUNT; i++) {
            // Scale back to int16 range
            float filtered_sample = floatBuffer[i] * maxInt;

            // Clamp to int16 range
            if (filtered_sample > maxInt) filtered_sample = maxInt;
            if (filtered_sample < minInt) filtered_sample = minInt;

            output_samples[i] = (int16_t) filtered_sample;
        }

        return SAMPLE_COUNT * sizeof(int16_t);
    }

} // namespace btspeaker 