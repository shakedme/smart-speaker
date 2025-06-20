#include "../include/microphone_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/memory_utils.hpp"

extern "C" {
#include "esp_log.h"
#include "board.h"
#include "i2s_stream.h"
#include "audio_common.h"
}

namespace btspeaker {

    MicrophoneManager::MicrophoneManager()
            : recording_(false), audioTaskHandle_(nullptr) {
        ESP_LOGI(TAG, "MicrophoneManager created for SPH0645LM4H I2S MEMS microphones");
    }

    MicrophoneManager::~MicrophoneManager() {
        shutdown();
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

        if (!micPipeline_) {
            ESP_LOGE(TAG, "Microphone pipeline not initialized");
            return ESP_ERR_INVALID_STATE;
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

        // Start pipeline
        esp_err_t ret = audio_pipeline_run(micPipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start microphone pipeline: %s", esp_err_to_name(ret));
            vTaskDelete(audioTaskHandle_);
            audioTaskHandle_ = nullptr;
            return ret;
        }

        ESP_LOGI(TAG, "Microphone recording started");
        return ESP_OK;
    }

    esp_err_t MicrophoneManager::stopRecording() {
        if (!recording_) {
            return ESP_OK;
        }

        recording_ = false;

        // Stop pipeline
        if (micPipeline_) {
            esp_err_t ret = audio_pipeline_stop(micPipeline_.get());
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to stop microphone pipeline: %s", esp_err_to_name(ret));
            }
        }

        // Stop task
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
        ESP_LOGI(TAG, "Audio processing task started for stereo I2S microphones");

        auto *audio_buffer = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
        if (!audio_buffer) {
            ESP_LOGE(TAG, "Failed to allocate audio buffer");
            return;
        }

        while (recording_) {
            // Read stereo audio from I2S microphone pipeline
            if (rawMicStream_) {
                int bytes_read = raw_stream_read(rawMicStream_.get(),
                                                 (char *) audio_buffer,
                                                 AUDIO_BUFFER_SIZE);

                if (bytes_read > 0 && audioCallback_) {
                    audioCallback_(audio_buffer, bytes_read);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent watchdog
        }

        free(audio_buffer);
        ESP_LOGI(TAG, "Audio processing task ended");
    }

    esp_err_t MicrophoneManager::setupMicrophonePipeline() {
        ESP_LOGI(TAG, "Setting up I2S microphone pipeline for SPH0645LM4H MEMS microphones");

        // Create event interface
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        audio_event_iface_handle_t evt_handle = audio_event_iface_init(&evt_cfg);
        if (!evt_handle) {
            ESP_LOGE(TAG, "Failed to create microphone event interface");
            return ESP_FAIL;
        }
        micEvent_.reset(evt_handle, audioEventIfaceDeleter);

        // Create I2S microphone stream for SPH0645LM4H MEMS microphones
        i2s_stream_cfg_t i2s_cfg = {
                .type = AUDIO_STREAM_READER,
                .transmit_mode = I2S_COMM_MODE_STD,
                .chan_cfg = {
                        .id = I2S_NUM_1, // Use I2S1 for microphones
                        .role = I2S_ROLE_MASTER,
                        .dma_desc_num = 3,    // Increased for better I2S performance
                        .dma_frame_num = 240, // Standard frame size for I2S
                        .auto_clear = true,
                        .auto_clear_before_cb = false,
                        .allow_pd = false,
                        .intr_priority = 0,
                },
                .std_cfg = {
                        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
                        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(MIC_BITS,
                                                                    I2S_SLOT_MODE_STEREO), // Stereo for two mics
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
                },
                .pdm_rx_cfg = {},
                .use_alc = false,
                .volume = 0,
                .out_rb_size = 4 * 1024,  // Ring buffer size for stereo I2S data
                .task_stack = 2048,       // Standard stack size for I2S processing
                .task_core = 1,
                .task_prio = 23,
                .stack_in_ext = true,     // Put I2S task stack in PSRAM
                .multi_out_num = 0,
                .uninstall_drv = false,
                .need_expand = false,
                .expand_src_bits = I2S_DATA_BIT_WIDTH_16BIT,
                .buffer_len = AUDIO_BUFFER_SIZE * 2, // Double buffer for stereo
        };

        audio_element_handle_t i2s_handle = i2s_stream_init(&i2s_cfg);
        if (!i2s_handle) {
            ESP_LOGE(TAG, "Failed to create I2S microphone stream");
            return ESP_FAIL;
        }
        i2SMicStream_.reset(i2s_handle, audioElementDeleter);

        // Create raw stream for audio processing
        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_WRITER,
                .out_rb_size = 8 * 1024,  // Larger buffer for stereo I2S data
        };

        audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
        if (!raw_handle) {
            ESP_LOGE(TAG, "Failed to create raw microphone stream");
            return ESP_FAIL;
        }
        rawMicStream_.reset(raw_handle, audioElementDeleter);

        // Create microphone pipeline
        audio_pipeline_cfg_t pipeline_cfg = {.rb_size = 4 * 1024};  // Ring buffer for pipeline
        audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline_handle) {
            ESP_LOGE(TAG, "Failed to create microphone pipeline");
            return ESP_FAIL;
        }
        micPipeline_.reset(pipeline_handle, audioPipelineDeleter);

        // Register elements
        esp_err_t ret = audio_pipeline_register(micPipeline_.get(), i2SMicStream_.get(), MIC_I2S);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register I2S mic stream: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = audio_pipeline_register(micPipeline_.get(), rawMicStream_.get(), MIC_RAW);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register raw mic stream: %s", esp_err_to_name(ret));
            return ret;
        }

        // Link elements
        const char *link_tag[] = {MIC_I2S, MIC_RAW};
        ret = audio_pipeline_link(micPipeline_.get(), link_tag, 2);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link microphone elements: %s", esp_err_to_name(ret));
            return ret;
        }

        // Set event listener
        ret = audio_pipeline_set_listener(micPipeline_.get(), micEvent_.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set microphone listener: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "I2S microphone pipeline setup completed for SPH0645LM4H");
        return ESP_OK;
    }

    esp_err_t MicrophoneManager::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up microphone pipeline");

        rawMicStream_.reset();
        i2SMicStream_.reset();
        micPipeline_.reset();
        micEvent_.reset();

        return ESP_OK;
    }

} // namespace btspeaker 