#include "../include/audio_response_player.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "board.h"
#include "audio_common.h"
#include "i2s_stream.h"
}

namespace btspeaker {

AudioResponsePlayer::AudioResponsePlayer()
    : state_(State::IDLE)
    , playback_task_handle_(nullptr)
    , audio_data_queue_(nullptr)
    , main_pipeline_manager_(nullptr)
    , main_pipeline_was_playing_(false)
    , buffer_position_(0)
    , auto_resume_main_pipeline_(true)
{
    ESP_LOGI(TAG, "AudioResponsePlayer created");
}

AudioResponsePlayer::~AudioResponsePlayer() {
    shutdown();
}

esp_err_t AudioResponsePlayer::init(AudioPipelineManager* main_pipeline_manager) {
    if (!main_pipeline_manager) {
        ESP_LOGE(TAG, "Main pipeline manager is null");
        return ESP_ERR_INVALID_ARG;
    }
    
    main_pipeline_manager_ = main_pipeline_manager;
    
    // Create audio data queue for streaming
    audio_data_queue_ = xQueueCreate(10, sizeof(AudioDataItem*));
    if (!audio_data_queue_) {
        ESP_LOGE(TAG, "Failed to create audio data queue");
        return ESP_ERR_NO_MEM;
    }
    
    // Setup response pipeline
    esp_err_t ret = setup_response_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup response pipeline: %s", esp_err_to_name(ret));
        return ret;
    }
    
    set_state(State::IDLE);
    ESP_LOGI(TAG, "Audio response player initialized successfully");
    return ESP_OK;
}

void AudioResponsePlayer::shutdown() {
    ESP_LOGI(TAG, "Shutting down audio response player");
    
    stop_playback();
    cleanup_response_pipeline();
    
    if (audio_data_queue_) {
        // Clear any remaining queue items
        AudioDataItem* item;
        while (xQueueReceive(audio_data_queue_, &item, 0) == pdTRUE) {
            delete item;
        }
        vQueueDelete(audio_data_queue_);
        audio_data_queue_ = nullptr;
    }
    
    set_state(State::IDLE);
}

esp_err_t AudioResponsePlayer::play_audio_response(const uint8_t* audio_data, size_t length) {
    if (!audio_data || length == 0) {
        ESP_LOGE(TAG, "Invalid audio data");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Playing audio response (%zu bytes)", length);
    
    // Queue the audio data for playback
    return queue_audio_chunk(audio_data, length, true);
}

esp_err_t AudioResponsePlayer::queue_audio_chunk(const uint8_t* audio_data, size_t length, bool is_final) {
    if (!audio_data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (state_.load() == State::IDLE) {
        // Start playback
        esp_err_t ret = temporarily_pause_main_pipeline();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to pause main pipeline: %s", esp_err_to_name(ret));
        }
        
        set_state(State::PREPARING);
        
        // Create playback task
        BaseType_t task_created = xTaskCreate(
            audio_playback_task,
            "audio_response",
            4096, // Stack size
            this,
            6,    // Higher priority than voice assistant
            &playback_task_handle_
        );
        
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio playback task");
            restore_main_pipeline();
            set_state(State::ERROR);
            return ESP_FAIL;
        }
        
        set_state(State::PLAYING);
    }
    
    // Create audio data item
    AudioDataItem* item = new AudioDataItem();
    item->data.assign(audio_data, audio_data + length);
    item->is_final = is_final;
    
    // Queue the data
    if (xQueueSend(audio_data_queue_, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to queue audio data, queue full");
        delete item;
        return ESP_ERR_TIMEOUT;
    }
    
    ESP_LOGD(TAG, "Queued audio chunk (%zu bytes, final: %s)", length, is_final ? "yes" : "no");
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::stop_playback() {
    ESP_LOGI(TAG, "Stopping audio playback");
    
    set_state(State::IDLE);
    
    // Stop playback task
    if (playback_task_handle_) {
        vTaskDelete(playback_task_handle_);
        playback_task_handle_ = nullptr;
    }
    
    // Clear audio queue
    if (audio_data_queue_) {
        AudioDataItem* item;
        while (xQueueReceive(audio_data_queue_, &item, 0) == pdTRUE) {
            delete item;
        }
    }
    
    // Restore main pipeline
    esp_err_t ret = restore_main_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore main pipeline: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::pause_playback() {
    if (state_.load() == State::PLAYING) {
        set_state(State::PAUSED);
        ESP_LOGI(TAG, "Audio playback paused");
    }
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::resume_playback() {
    if (state_.load() == State::PAUSED) {
        set_state(State::PLAYING);
        ESP_LOGI(TAG, "Audio playback resumed");
    }
    return ESP_OK;
}

void AudioResponsePlayer::audio_playback_task(void* arg) {
    AudioResponsePlayer* self = static_cast<AudioResponsePlayer*>(arg);
    self->run_audio_playback();
    vTaskDelete(NULL);
}

void AudioResponsePlayer::run_audio_playback() {
    ESP_LOGI(TAG, "Audio playback task started");
    
    // Start response pipeline
    if (response_pipeline_) {
        esp_err_t ret = audio_pipeline_run(response_pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start response pipeline: %s", esp_err_to_name(ret));
            set_state(State::ERROR);
            return;
        }
    }
    
    bool playback_complete = false;
    
    while (state_.load() != State::IDLE && !playback_complete) {
        AudioDataItem* item = nullptr;
        
        // Wait for audio data
        if (xQueueReceive(audio_data_queue_, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item && state_.load() == State::PLAYING) {
                // Convert and write audio data
                std::vector<uint8_t> converted_audio;
                esp_err_t ret = convert_audio_format(item->data.data(), item->data.size(), converted_audio);
                
                if (ret == ESP_OK && !converted_audio.empty()) {
                    ret = write_audio_to_pipeline(converted_audio.data(), converted_audio.size());
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to write audio to pipeline: %s", esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to convert audio format");
                }
                
                if (item->is_final) {
                    playback_complete = true;
                    ESP_LOGI(TAG, "Final audio chunk processed, playback complete");
                }
            }
            
            delete item;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Stop response pipeline
    if (response_pipeline_) {
        audio_pipeline_stop(response_pipeline_.get());
    }
    
    // Auto-restore main pipeline if enabled
    if (auto_resume_main_pipeline_) {
        restore_main_pipeline();
    }
    
    set_state(State::IDLE);
    ESP_LOGI(TAG, "Audio playback task ended");
}

esp_err_t AudioResponsePlayer::setup_response_pipeline() {
    ESP_LOGI(TAG, "Setting up response audio pipeline");
    
    // Create event interface
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt_handle = audio_event_iface_init(&evt_cfg);
    if (!evt_handle) {
        ESP_LOGE(TAG, "Failed to create response event interface");
        return ESP_FAIL;
    }
    response_evt_.reset(evt_handle, audio_event_iface_deleter);
    
    // Create raw stream for audio input
    raw_stream_cfg_t raw_cfg = {
        .type = AUDIO_STREAM_READER,
        .out_rb_size = 4 * 1024,  // Reduced from 8KB to save memory
    };
    
    audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
    if (!raw_handle) {
        ESP_LOGE(TAG, "Failed to create raw response stream");
        return ESP_FAIL;
    }
    raw_response_stream_.reset(raw_handle, audio_element_deleter);
    
    // Create I2S output stream (same configuration as main pipeline for compatibility)
    i2s_stream_cfg_t i2s_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .transmit_mode = I2S_COMM_MODE_STD,
        .chan_cfg = {
            .id = I2S_NUM_0, // Same I2S port as main pipeline
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 2,    // Reduced from 3 to save memory
            .dma_frame_num = 200, // Reduced from 312 to save memory
            .auto_clear = true,
            .auto_clear_before_cb = false,
            .allow_pd = false,
            .intr_priority = 0,
        },
        .std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RESPONSE_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(RESPONSE_BITS, 
                       static_cast<i2s_slot_mode_t>(RESPONSE_CHANNELS == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO)),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = static_cast<gpio_num_t>(I2S_BCK_IO),
                .ws   = static_cast<gpio_num_t>(I2S_WS_IO),
                .dout = static_cast<gpio_num_t>(I2S_DO_IO),
                .din  = GPIO_NUM_NC,
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
        .out_rb_size = 4 * 1024,  // Reduced from 8KB to save memory
        .task_stack = 2560,       // Reduced from 3584 to save memory
        .task_core = 0,
        .task_prio = 24, // Higher priority than main pipeline
        .stack_in_ext = false,
        .multi_out_num = 0,
        .uninstall_drv = false, // Don't uninstall I2S driver (shared with main pipeline)
        .need_expand = false,
        .expand_src_bits = I2S_DATA_BIT_WIDTH_16BIT,
        .buffer_len = AUDIO_CHUNK_SIZE,
    };
    
    audio_element_handle_t i2s_handle = i2s_stream_init(&i2s_cfg);
    if (!i2s_handle) {
        ESP_LOGE(TAG, "Failed to create I2S response stream");
        return ESP_FAIL;
    }
    i2s_response_writer_.reset(i2s_handle, audio_element_deleter);
    
    // Create response pipeline
    audio_pipeline_cfg_t pipeline_cfg = { .rb_size = 4 * 1024 };  // Reduced from 8KB to save memory
    audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline_handle) {
        ESP_LOGE(TAG, "Failed to create response pipeline");
        return ESP_FAIL;
    }
    response_pipeline_.reset(pipeline_handle, audio_pipeline_deleter);
    
    // Register elements
    esp_err_t ret = audio_pipeline_register(response_pipeline_.get(), raw_response_stream_.get(), "raw");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register raw response stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = audio_pipeline_register(response_pipeline_.get(), i2s_response_writer_.get(), "i2s");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S response stream: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Link elements
    const char* link_tag[] = {"raw", "i2s"};
    ret = audio_pipeline_link(response_pipeline_.get(), link_tag, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link response elements: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set event listener
    ret = audio_pipeline_set_listener(response_pipeline_.get(), response_evt_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response listener: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Response audio pipeline setup completed");
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::cleanup_response_pipeline() {
    ESP_LOGI(TAG, "Cleaning up response pipeline");
    
    if (response_pipeline_) {
        audio_pipeline_stop(response_pipeline_.get());
        audio_pipeline_terminate(response_pipeline_.get());
    }
    
    i2s_response_writer_.reset();
    raw_response_stream_.reset();
    response_pipeline_.reset();
    response_evt_.reset();
    
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::temporarily_pause_main_pipeline() {
    if (!main_pipeline_manager_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if main pipeline is playing
    main_pipeline_was_playing_ = main_pipeline_manager_->is_pipeline_ready();
    
    if (main_pipeline_was_playing_) {
        ESP_LOGI(TAG, "Temporarily pausing main pipeline for response playback");
        esp_err_t ret = main_pipeline_manager_->pause_pipeline();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to pause main pipeline: %s", esp_err_to_name(ret));
            main_pipeline_was_playing_ = false;
        }
    }
    
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::restore_main_pipeline() {
    if (!main_pipeline_manager_ || !main_pipeline_was_playing_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Restoring main pipeline after response playback");
    esp_err_t ret = main_pipeline_manager_->resume_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resume main pipeline: %s", esp_err_to_name(ret));
    }
    
    main_pipeline_was_playing_ = false;
    return ret;
}

esp_err_t AudioResponsePlayer::write_audio_to_pipeline(const uint8_t* data, size_t length) {
    if (!raw_response_stream_ || !data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int bytes_written = raw_stream_write(raw_response_stream_.get(), (char*)data, length);
    if (bytes_written != length) {
        ESP_LOGW(TAG, "Incomplete write: %d/%zu bytes", bytes_written, length);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t AudioResponsePlayer::convert_audio_format(const uint8_t* input, size_t input_len, 
                                                   std::vector<uint8_t>& output) {
    if (!input || input_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // For now, assume input is already in the correct format (16-bit PCM)
    // In a more sophisticated implementation, you might need sample rate conversion
    // or channel conversion here
    
    output.assign(input, input + input_len);
    return ESP_OK;
}

void AudioResponsePlayer::set_state(State new_state) {
    State old_state = state_.exchange(new_state);
    if (old_state != new_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
    }
}

} // namespace btspeaker 