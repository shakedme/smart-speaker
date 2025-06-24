#include "../include/llm_pipeline.hpp"
#include "../include/audio_pipeline_util.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "audio_common.h"
#include "i2s_stream.h"
}

namespace btspeaker {

    LLMPipeline::LLMPipeline()
            : state_(State::IDLE), playbackTaskHandle_(nullptr) {
        ESP_LOGI(TAG, "Initializing LLM pipeline");
//
//        // Create audio data queue for streaming
//        audioDataQueue_ = xQueueCreate(10, sizeof(AudioDataItem *));
//        if (!audioDataQueue_) {
//            ESP_LOGE(TAG, "Failed to create audio data queue");
//            throw std::runtime_error("Failed to create audio data queue");
//        }

        // Setup response pipeline
        setState(State::IDLE);
        ESP_LOGI(TAG, "LLM pipeline initialized successfully");
    }

    LLMPipeline::~LLMPipeline() {
        shutdown();
    }

    void LLMPipeline::shutdown() {
        ESP_LOGI(TAG, "Shutting down LLM pipeline");

        stop();
//
//        if (audioDataQueue_) {
//            // Clear any remaining queue items
//            AudioDataItem *item;
//            while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
//                delete item;
//            }
//            vQueueDelete(audioDataQueue_);
//            audioDataQueue_ = nullptr;
//        }

        setState(State::IDLE);
    }

    esp_err_t LLMPipeline::queueAudioChunk(const uint8_t *audio_data, size_t length) {
//        if (state_.load() == State::IDLE) {
//            ESP_LOGI(TAG, "Starting LLM audio playback");
//
//            setState(State::PREPARING);
//
//
//            // Create playback task
//            BaseType_t task_created = xTaskCreate(
//                    audioPlaybackTask,
//                    "llm_audio",
//                    4096, // Stack size
//                    this,
//                    6,    // Higher priority than voice assistant
//                    &playbackTaskHandle_
//            );
//
//            if (task_created != pdPASS) {
//                ESP_LOGE(TAG, "Failed to create audio playback task");
//                setState(State::ERROR);
//                return ESP_FAIL;
//            }
//
//            setState(State::PLAYING);
//        }
//
//        // Create audio data item
//        auto *item = new AudioDataItem();
//        item->data.assign(audio_data, audio_data + length);
//
//        // Queue the data
//        if (xQueueSend(audioDataQueue_, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
//            ESP_LOGW(TAG, "Failed to queue audio data, queue full");
//            delete item;
//            return ESP_ERR_TIMEOUT;
//        }
//
//        ESP_LOGD(TAG, "Queued audio chunk (%zu bytes)", length);
        writeAudioToPipeline(audio_data, length);
        return ESP_OK;
    }

    esp_err_t LLMPipeline::stop() {
        ESP_LOGI(TAG, "Stopping LLM audio playback");

        setState(State::IDLE);
//
//        // Stop playback task
//        if (playbackTaskHandle_) {
//            vTaskDelete(playbackTaskHandle_);
//            playbackTaskHandle_ = nullptr;
//        }
//
//        // Clear audio queue
//        if (audioDataQueue_) {
//            AudioDataItem *item;
//            while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
//                delete item;
//            }
//        }

        AudioPipelineUtil::cleanupPipeline(*audioPipeline);

        return ESP_OK;
    }

    esp_err_t LLMPipeline::pause() {
        if (state_.load() == State::PLAYING) {
            setState(State::PAUSED);
            ESP_LOGI(TAG, "LLM audio playback paused");
        }
        return ESP_OK;
    }

    esp_err_t LLMPipeline::resume() {
        if (state_.load() == State::PAUSED) {
            setState(State::PLAYING);
            ESP_LOGI(TAG, "LLM audio playback resumed");
        }
        return ESP_OK;
    }

    void LLMPipeline::audioPlaybackTask(void *arg) {
        auto *self = static_cast<LLMPipeline *>(arg);
        self->runAudioPlayback();
        vTaskDelete(NULL);
    }

    void LLMPipeline::runAudioPlayback() {
        ESP_LOGI(TAG, "LLM audio playback task started");

        while (state_.load() != State::IDLE) {
            AudioDataItem *item = nullptr;

            // Wait for audio data
            if (xQueueReceive(audioDataQueue_, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (item && state_.load() == State::PLAYING) {
                    esp_err_t ret = writeAudioToPipeline(item->data.data(), item->data.size());
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to write audio to pipeline: %s", esp_err_to_name(ret));
                    }
                }

                delete item;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        AudioPipelineUtil::cleanupPipeline(*audioPipeline);

        setState(State::IDLE);
        ESP_LOGI(TAG, "LLM audio playback task ended");
    }

    esp_err_t LLMPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up LLM response audio pipeline (24kHz mono 16-bit)");

        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = 16 * 1024,
        };

        audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
        if (!raw_handle) {
            ESP_LOGE(TAG, "Failed to create raw response stream");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Creating I2S pipeline with: %dHz, %dch, %dbit", 
                 DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, DEFAULT_BITS);

        audioPipeline = AudioPipelineUtil::createAudioPipeline(
                AudioSource::VOICE_RESPONSE,
                raw_handle,
                false,
                DEFAULT_SAMPLE_RATE,
                DEFAULT_CHANNELS,
                DEFAULT_BITS
        );

        audio_pipeline_run(audioPipeline->pipeline.get());

        ESP_LOGI(TAG, "LLM response audio pipeline setup completed successfully");
        return ESP_OK;
    }

    esp_err_t LLMPipeline::writeAudioToPipeline(const uint8_t *data, size_t length) {
        if (!audioPipeline->pipeline || !data || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        int bytes_written = raw_stream_write(audioPipeline->i2SStreamReader.get(), (char *) data, length);
        if (bytes_written != length) {
            ESP_LOGW(TAG, "Incomplete write: %d/%zu bytes", bytes_written, length);
            return ESP_FAIL;
        }

        return ESP_OK;
    }

    void LLMPipeline::setState(State new_state) {
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        }
    }

} // namespace btspeaker 