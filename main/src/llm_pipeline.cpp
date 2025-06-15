#include "../include/llm_pipeline.hpp"
#include "../include/voice_config.hpp"
#include "../include/i2s_audio_service.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "board.h"
#include "audio_common.h"
#include "i2s_stream.h"
}

namespace btspeaker {

LLMPipeline::LLMPipeline()
    : state_(State::IDLE)
    , playbackTaskHandle_(nullptr)
{
    ESP_LOGI(TAG, "LLMPipeline created");
}

LLMPipeline::~LLMPipeline() {
    shutdown();
}

esp_err_t LLMPipeline::init() {
    ESP_LOGI(TAG, "Initializing LLM pipeline");

    // Create audio data queue for streaming
    audioDataQueue_ = xQueueCreate(10, sizeof(AudioDataItem*));
    if (!audioDataQueue_) {
        ESP_LOGE(TAG, "Failed to create audio data queue");
        return ESP_ERR_NO_MEM;
    }

    // Setup response pipeline
    esp_err_t ret = setupResponsePipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup response pipeline: %s", esp_err_to_name(ret));
        return ret;
    }

    set_state(State::IDLE);
    ESP_LOGI(TAG, "LLM pipeline initialized successfully");
    return ESP_OK;
}

void LLMPipeline::shutdown() {
    ESP_LOGI(TAG, "Shutting down LLM pipeline");

    stopPlayback();
    cleanupResponsePipeline();

    if (audioDataQueue_) {
        // Clear any remaining queue items
        AudioDataItem* item;
        while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
            delete item;
        }
        vQueueDelete(audioDataQueue_);
        audioDataQueue_ = nullptr;
    }

    set_state(State::IDLE);
}

esp_err_t LLMPipeline::queueAudioChunk(const uint8_t* audio_data, size_t length, bool is_final) {
    if (!audio_data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state_.load() == State::IDLE) {
        ESP_LOGI(TAG, "Starting LLM audio playback");
        set_state(State::PREPARING);

        // Create playback task
        BaseType_t task_created = xTaskCreate(
                audioPlaybackTask,
                "llm_audio",
                4096, // Stack size
                this,
                6,    // Higher priority than voice assistant
                &playbackTaskHandle_
        );

        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio playback task");
            set_state(State::ERROR);
            return ESP_FAIL;
        }

        set_state(State::PLAYING);
    }

    // Create audio data item
    auto* item = new AudioDataItem();
    item->data.assign(audio_data, audio_data + length);
    item->isFinal = is_final;

    // Queue the data
    if (xQueueSend(audioDataQueue_, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to queue audio data, queue full");
        delete item;
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGD(TAG, "Queued audio chunk (%zu bytes, final: %s)", length, is_final ? "yes" : "no");
    return ESP_OK;
}

esp_err_t LLMPipeline::stopPlayback() {
    ESP_LOGI(TAG, "Stopping LLM audio playback");

    set_state(State::IDLE);

    // Stop playback task
    if (playbackTaskHandle_) {
        vTaskDelete(playbackTaskHandle_);
        playbackTaskHandle_ = nullptr;
    }

    // Clear audio queue
    if (audioDataQueue_) {
        AudioDataItem* item;
        while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
            delete item;
        }
    }

    return ESP_OK;
}

esp_err_t LLMPipeline::pausePlayback() {
    if (state_.load() == State::PLAYING) {
        set_state(State::PAUSED);
        ESP_LOGI(TAG, "LLM audio playback paused");
    }
    return ESP_OK;
}

esp_err_t LLMPipeline::resumePlayback() {
    if (state_.load() == State::PAUSED) {
        set_state(State::PLAYING);
        ESP_LOGI(TAG, "LLM audio playback resumed");
    }
    return ESP_OK;
}

void LLMPipeline::audioPlaybackTask(void* arg) {
    auto* self = static_cast<LLMPipeline*>(arg);
    self->runAudioPlayback();
    vTaskDelete(NULL);
}

void LLMPipeline::runAudioPlayback() {
    ESP_LOGI(TAG, "LLM audio playback task started");

    // Start response pipeline
    if (responsePipeline_) {
        esp_err_t ret = audio_pipeline_run(responsePipeline_.get());
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
        if (xQueueReceive(audioDataQueue_, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item && state_.load() == State::PLAYING) {
                // Convert and write audio data
                std::vector<uint8_t> converted_audio;
                esp_err_t ret = convertAudioFormat(item->data.data(), item->data.size(), converted_audio);

                if (ret == ESP_OK && !converted_audio.empty()) {
                    ret = writeAudioToPipeline(converted_audio.data(), converted_audio.size());
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to write audio to pipeline: %s", esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to convert audio format");
                }

                if (item->isFinal) {
                    playback_complete = true;
                    ESP_LOGI(TAG, "Final audio chunk processed, playback complete");
                }
            }

            delete item;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Stop response pipeline
    if (responsePipeline_) {
        audio_pipeline_stop(responsePipeline_.get());
    }

    set_state(State::IDLE);
    ESP_LOGI(TAG, "LLM audio playback task ended");
}

esp_err_t LLMPipeline::setupResponsePipeline() {
    ESP_LOGI(TAG, "Setting up LLM response audio pipeline");

    // Create event interface
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt_handle = audio_event_iface_init(&evt_cfg);
    if (!evt_handle) {
        ESP_LOGE(TAG, "Failed to create response event interface");
        return ESP_FAIL;
    }
    responseEvent_.reset(evt_handle, audioEventIfaceDeleter);

    // Create raw stream for audio input
    raw_stream_cfg_t raw_cfg = {
        .type = AUDIO_STREAM_READER,
        .out_rb_size = 4 * 1024,  // 4KB buffer
    };

    audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
    if (!raw_handle) {
        ESP_LOGE(TAG, "Failed to create raw response stream");
        return ESP_FAIL;
    }
    rawResponseStream_.reset(raw_handle, audioElementDeleter);

    // Use shared I2S service for consistent configuration
    audio_element_handle_t i2s_handle = I2SAudioService::getInstance().createI2SWriter(
        I2SAudioService::AudioSource::VOICE_RESPONSE,
        DEFAULT_SAMPLE_RATE, // 24000 Hz (Gemini typical output)
        DEFAULT_CHANNELS,    // 1 (mono)
        DEFAULT_BITS         // 16 bits
    );

    if (!i2s_handle) {
        ESP_LOGE(TAG, "Failed to create I2S response stream via shared service");
        return ESP_FAIL;
    }
    i2sResponseWriter_.reset(i2s_handle, audioElementDeleter);

    // Create response pipeline
    audio_pipeline_cfg_t pipeline_cfg = { .rb_size = 4 * 1024 };
    audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline_handle) {
        ESP_LOGE(TAG, "Failed to create response pipeline");
        return ESP_FAIL;
    }
    responsePipeline_.reset(pipeline_handle, audioPipelineDeleter);

    // Register elements
    esp_err_t ret = audio_pipeline_register(responsePipeline_.get(), rawResponseStream_.get(), "raw");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register raw response stream: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = audio_pipeline_register(responsePipeline_.get(), i2sResponseWriter_.get(), "i2s");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S response stream: %s", esp_err_to_name(ret));
        return ret;
    }

    // Link elements
    const char* link_tag[] = {"raw", "i2s"};
    ret = audio_pipeline_link(responsePipeline_.get(), link_tag, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link response elements: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set event listener
    ret = audio_pipeline_set_listener(responsePipeline_.get(), responseEvent_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response listener: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LLM response audio pipeline setup completed");
    return ESP_OK;
}

esp_err_t LLMPipeline::cleanupResponsePipeline() {
    ESP_LOGI(TAG, "Cleaning up LLM response pipeline");

    if (responsePipeline_) {
        audio_pipeline_stop(responsePipeline_.get());
        audio_pipeline_terminate(responsePipeline_.get());
    }

    i2sResponseWriter_.reset();
    rawResponseStream_.reset();
    responsePipeline_.reset();
    responseEvent_.reset();

    return ESP_OK;
}

esp_err_t LLMPipeline::writeAudioToPipeline(const uint8_t* data, size_t length) {
    if (!rawResponseStream_ || !data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int bytes_written = raw_stream_write(rawResponseStream_.get(), (char*)data, length);
    if (bytes_written != length) {
        ESP_LOGW(TAG, "Incomplete write: %d/%zu bytes", bytes_written, length);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t LLMPipeline::convertAudioFormat(const uint8_t* input, size_t input_len,
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

void LLMPipeline::set_state(State new_state) {
    State old_state = state_.exchange(new_state);
    if (old_state != new_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
    }
}

} // namespace btspeaker 