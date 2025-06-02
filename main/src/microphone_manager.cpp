#include "../include/microphone_manager.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "board.h"
#include "i2s_stream.h"
#include "audio_common.h"
}

namespace btspeaker {

MicrophoneManager::MicrophoneManager()
    : recording_(false)
    , audio_task_handle_(nullptr)
{
    ESP_LOGI(TAG, "MicrophoneManager created");
}

MicrophoneManager::~MicrophoneManager() {
    shutdown();
}

esp_err_t MicrophoneManager::init() {
    ESP_LOGI(TAG, "Initializing microphone manager");
    
    esp_err_t ret = setup_microphone_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup microphone pipeline: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Microphone manager initialized successfully");
    return ESP_OK;
}

esp_err_t MicrophoneManager::start_recording() {
    if (recording_) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_OK;
    }
    
    if (!mic_pipeline_) {
        ESP_LOGE(TAG, "Microphone pipeline not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create audio processing task
    BaseType_t task_created = xTaskCreate(
        audio_task,
        "mic_audio",
        4096, // Stack size
        this,
        5,    // Priority
        &audio_task_handle_
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        return ESP_FAIL;
    }
    
    // Start pipeline
    esp_err_t ret = audio_pipeline_run(mic_pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start microphone pipeline: %s", esp_err_to_name(ret));
        vTaskDelete(audio_task_handle_);
        audio_task_handle_ = nullptr;
        return ret;
    }
    
    recording_ = true;
    ESP_LOGI(TAG, "Microphone recording started");
    return ESP_OK;
}

esp_err_t MicrophoneManager::stop_recording() {
    if (!recording_) {
        return ESP_OK;
    }
    
    recording_ = false;
    
    // Stop pipeline
    if (mic_pipeline_) {
        esp_err_t ret = audio_pipeline_stop(mic_pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop microphone pipeline: %s", esp_err_to_name(ret));
        }
    }
    
    // Stop task
    if (audio_task_handle_) {
        vTaskDelete(audio_task_handle_);
        audio_task_handle_ = nullptr;
    }
    
    ESP_LOGI(TAG, "Microphone recording stopped");
    return ESP_OK;
}

void MicrophoneManager::shutdown() {
    ESP_LOGI(TAG, "Shutting down microphone manager");
    
    stop_recording();
    cleanup_pipeline();
}

void MicrophoneManager::set_audio_callback(AudioDataCallback callback) {
    audio_callback_ = callback;
}

void MicrophoneManager::audio_task(void* arg) {
    MicrophoneManager* self = static_cast<MicrophoneManager*>(arg);
    self->run_audio_processing();
    vTaskDelete(NULL);
}

void MicrophoneManager::run_audio_processing() {
    ESP_LOGI(TAG, "Audio processing task started");
    
    uint8_t* audio_buffer = new uint8_t[AUDIO_BUFFER_SIZE];
    
    while (recording_) {
        // Read audio from microphone pipeline
        if (raw_mic_stream_) {
            int bytes_read = raw_stream_read(raw_mic_stream_.get(), 
                                           (char*)audio_buffer, 
                                           AUDIO_BUFFER_SIZE);
            
            if (bytes_read > 0 && audio_callback_) {
                audio_callback_(audio_buffer, bytes_read);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent watchdog
    }
    
    delete[] audio_buffer;
    ESP_LOGI(TAG, "Audio processing task ended");
}

esp_err_t MicrophoneManager::setup_microphone_pipeline() {
    ESP_LOGI(TAG, "Setting up microphone pipeline");
    
    // Create event interface
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt_handle = audio_event_iface_init(&evt_cfg);
    if (!evt_handle) {
        ESP_LOGE(TAG, "Failed to create microphone event interface");
        return ESP_FAIL;
    }
    mic_evt_.reset(evt_handle, audio_event_iface_deleter);
    
    // Create I2S microphone stream (use ADC for MAX9814)
    // Note: MAX9814 outputs analog, so we use ADC input instead of I2S
    i2s_stream_cfg_t i2s_cfg = {
        .type = AUDIO_STREAM_READER,
        .transmit_mode = I2S_COMM_MODE_STD,
        .chan_cfg = {
            .id = I2S_NUM_1, // Use I2S1 for microphone to avoid conflict
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 3,
            .dma_frame_num = 240,
            .auto_clear = true,
            .auto_clear_before_cb = false,
            .allow_pd = false,
            .intr_priority = 0,
        },
        .std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(MIC_BITS, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = GPIO_NUM_NC,
                .ws = GPIO_NUM_NC,
                .dout = GPIO_NUM_NC,
                .din = GPIO_NUM_35, // MAX9814 connected to GPIO35 (ADC)
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        },
#if SOC_I2S_SUPPORTS_PDM_RX
        .pdm_rx_cfg = {},
#endif
#if SOC_I2S_SUPPORTS_PDM_TX
        .pdm_tx_cfg = {},
#endif
        .use_alc = false,
        .volume = 0,
        .out_rb_size = 4 * 1024,
        .task_stack = 2048,
        .task_core = 1,
        .task_prio = 23,
        .stack_in_ext = false,
        .multi_out_num = 0,
        .uninstall_drv = false,
        .need_expand = false,
        .expand_src_bits = I2S_DATA_BIT_WIDTH_16BIT,
        .buffer_len = AUDIO_BUFFER_SIZE,
    };
    
    audio_element_handle_t i2s_handle = i2s_stream_init(&i2s_cfg);
    if (!i2s_handle) {
        ESP_LOGE(TAG, "Failed to create I2S microphone stream");
        return ESP_FAIL;
    }
    i2s_mic_stream_.reset(i2s_handle, audio_element_deleter);
    
    // Create raw stream for audio processing
    raw_stream_cfg_t raw_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .out_rb_size = 8 * 1024,
    };
    
    audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
    if (!raw_handle) {
        ESP_LOGE(TAG, "Failed to create raw microphone stream");
        return ESP_FAIL;
    }
    raw_mic_stream_.reset(raw_handle, audio_element_deleter);
    
    // Create microphone pipeline
    audio_pipeline_cfg_t pipeline_cfg = { .rb_size = 4 * 1024 };
    audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline_handle) {
        ESP_LOGE(TAG, "Failed to create microphone pipeline");
        return ESP_FAIL;
    }
    mic_pipeline_.reset(pipeline_handle, audio_pipeline_deleter);
    
    // Register elements
    esp_err_t ret = audio_pipeline_register(mic_pipeline_.get(), i2s_mic_stream_.get(), "mic_i2s");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S mic stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = audio_pipeline_register(mic_pipeline_.get(), raw_mic_stream_.get(), "mic_raw");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register raw mic stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Link elements
    const char* link_tag[] = {"mic_i2s", "mic_raw"};
    ret = audio_pipeline_link(mic_pipeline_.get(), link_tag, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link microphone elements: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set event listener
    ret = audio_pipeline_set_listener(mic_pipeline_.get(), mic_evt_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set microphone listener: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Microphone pipeline setup completed");
    return ESP_OK;
}

esp_err_t MicrophoneManager::cleanup_pipeline() {
    ESP_LOGI(TAG, "Cleaning up microphone pipeline");
    
    raw_mic_stream_.reset();
    i2s_mic_stream_.reset();
    mic_pipeline_.reset();
    mic_evt_.reset();
    
    return ESP_OK;
}

} // namespace btspeaker 