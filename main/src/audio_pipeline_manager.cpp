#include "../include/audio_pipeline_manager.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "bluetooth_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

static constexpr const char* TAG = "PIPELINE_MGR";

esp_err_t AudioPipelineManager::setup_pipeline() {
    ESP_LOGI(TAG, "Setting up audio pipeline");
    
    // Initialize event interface with RAII
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt_handle = audio_event_iface_init(&evt_cfg);
    if (!evt_handle) {
        ESP_LOGE(TAG, "Failed to create audio event interface");
        return ESP_FAIL;
    }
    evt_.reset(evt_handle, audio_event_iface_deleter);
    
    // Start Bluetooth service
    bluetooth_service_cfg_t bt_cfg = {
        .device_name = "ESP32_SPEAKER",
        .remote_name = nullptr,
        .mode = BLUETOOTH_A2DP_SINK,
        .user_callback = {}
    };
    esp_err_t ret = bluetooth_service_start(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Bluetooth service: %s", esp_err_to_name(ret));
        return ret;
    }
    bt_service_started_ = true;
    ESP_LOGI(TAG, "Bluetooth A2DP Sink started, device name: %s", bt_cfg.device_name);

    // Create I2S stream with safe configuration
    i2s_stream_cfg_t i2s_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .transmit_mode = I2S_COMM_MODE_STD,
        .chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 3,
            .dma_frame_num = 312,
            .auto_clear = true,
            .auto_clear_before_cb = false,
            .allow_pd = false,
            .intr_priority = 0
        },
        .std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = static_cast<gpio_num_t>(I2S_BCK_IO),
                .ws   = static_cast<gpio_num_t>(I2S_WS_IO),
                .dout = static_cast<gpio_num_t>(I2S_DO_IO),
                .din  = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false
                }
            },
        },
        .use_alc = false,
        .volume = 0,
        .out_rb_size = 8 * 1024,
        .task_stack = 3584,
        .task_core = 0,
        .task_prio = 23,
        .stack_in_ext = false,
        .multi_out_num = 0,
        .uninstall_drv = true,
        .need_expand = false,
        .expand_src_bits = I2S_DATA_BIT_WIDTH_16BIT,
        .buffer_len = 3600
    };
    
    audio_element_handle_t i2s_handle = i2s_stream_init(&i2s_cfg);
    if (!i2s_handle) {
        ESP_LOGE(TAG, "Failed to create I2S stream");
        return ESP_FAIL;
    }
    i2s_stream_writer_.reset(i2s_handle, audio_element_deleter);
    
    // Create Bluetooth stream
    audio_element_handle_t bt_handle = bluetooth_service_create_stream();
    if (!bt_handle) {
        ESP_LOGE(TAG, "Failed to create Bluetooth stream");
        return ESP_FAIL;
    }
    bt_stream_reader_.reset(bt_handle, audio_element_deleter);
    
    // Create pipeline
    audio_pipeline_cfg_t pipeline_cfg = { .rb_size = 8 * 1024 };
    audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline_handle) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        return ESP_FAIL;
    }
    pipeline_.reset(pipeline_handle, audio_pipeline_deleter);
    
    // Register and link elements
    ret = audio_pipeline_register(pipeline_.get(), bt_stream_reader_.get(), "bt");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BT stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = audio_pipeline_register(pipeline_.get(), i2s_stream_writer_.get(), "i2s");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    const char* link_tag[] = {"bt", "i2s"};
    ret = audio_pipeline_link(pipeline_.get(), link_tag, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link pipeline elements: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set event listener
    ret = audio_pipeline_set_listener(pipeline_.get(), evt_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pipeline listener: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Audio pipeline setup completed successfully");
    return ESP_OK;
}

esp_err_t AudioPipelineManager::start_pipeline() {
    if (!pipeline_) {
        ESP_LOGE(TAG, "Pipeline not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = audio_pipeline_run(pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Audio pipeline started successfully");
    return ESP_OK;
}

esp_err_t AudioPipelineManager::stop_pipeline() {
    if (!pipeline_) {
        ESP_LOGW(TAG, "Pipeline not available for stop");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = audio_pipeline_stop(pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop pipeline: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t AudioPipelineManager::pause_pipeline() {
    if (!pipeline_) {
        ESP_LOGW(TAG, "Pipeline not available for pause");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = audio_pipeline_pause(pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t AudioPipelineManager::resume_pipeline() {
    if (!pipeline_) {
        ESP_LOGW(TAG, "Pipeline not available for resume");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = audio_pipeline_resume(pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t AudioPipelineManager::run_event_loop() {
    if (!evt_) {
        ESP_LOGE(TAG, "Event interface not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting audio event loop");
    
    while (true) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt_.get(), &msg, pdMS_TO_TICKS(1000));
        
        if (ret == ESP_ERR_TIMEOUT) {
            continue; // Normal timeout, continue listening
        }
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Event listen error: %s", esp_err_to_name(ret));
            break;
        }
        
        if (!handle_audio_event(msg)) {
            ESP_LOGI(TAG, "Event handler requested exit");
            break; // Event handler requested exit
        }
    }
    
    return ESP_OK;
}

bool AudioPipelineManager::handle_audio_event(const audio_event_iface_msg_t& msg) {
    // Handle Bluetooth music info updates
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && 
        msg.source == bt_stream_reader_.get() &&
        msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        
        audio_element_info_t music_info = {
            .sample_rates = 0,
            .channels = 0,
            .bits = 0,
            .bps = 0,
            .byte_pos = 0,
            .total_bytes = 0,
            .duration = 0,
            .uri = nullptr,
            .codec_fmt = ESP_CODEC_TYPE_UNKNOW,
            .reserve_data = {}
        };
        esp_err_t ret = audio_element_getinfo(bt_stream_reader_.get(), &music_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Music info: sample_rate=%d, bits=%d, channels=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);
            
            // Validate parameters before setting
            if (music_info.sample_rates > 0 && music_info.bits > 0 && music_info.channels > 0) {
                ret = i2s_stream_set_clk(i2s_stream_writer_.get(), 
                                       music_info.sample_rates, 
                                       music_info.bits, 
                                       music_info.channels);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to update I2S clock: %s", esp_err_to_name(ret));
                }
            }
        }
        return true;
    }
    
    // Handle stop events
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && 
        msg.source == i2s_stream_writer_.get() && 
        msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
        
        int state = static_cast<int>(reinterpret_cast<intptr_t>(msg.data));
        if (state == AEL_STATUS_STATE_STOPPED || state == AEL_STATUS_STATE_FINISHED) {
            ESP_LOGW(TAG, "I2S stream stopped or finished");
            return false; // Request event loop exit
        }
    }
    
    return true; // Continue event loop
}

void AudioPipelineManager::cleanup() {
    ESP_LOGI(TAG, "Cleaning up audio pipeline");
    
    // Stop pipeline first
    if (pipeline_) {
        audio_pipeline_stop(pipeline_.get());
        audio_pipeline_terminate(pipeline_.get());
    }
    
    // Clean up in reverse order of initialization
    i2s_stream_writer_.reset();
    bt_stream_reader_.reset();
    pipeline_.reset();
    evt_.reset();
    
    // Stop Bluetooth service
    if (bt_service_started_) {
        bluetooth_service_destroy();
        bt_service_started_ = false;
        ESP_LOGI(TAG, "Bluetooth service stopped");
    }
    
    ESP_LOGI(TAG, "Audio pipeline cleanup complete");
}

} // namespace btspeaker 