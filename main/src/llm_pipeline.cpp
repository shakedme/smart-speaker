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

        // Create audio data queue for streaming
        audioDataQueue_ = xQueueCreate(10, sizeof(AudioDataItem *));
        if (!audioDataQueue_) {
            ESP_LOGE(TAG, "Failed to create audio data queue");
            throw std::runtime_error("Failed to create audio data queue");
        }

        // Setup response pipeline
        esp_err_t ret = setupResponsePipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup response pipeline: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize LLM pipeline");
        }

        getState(State::IDLE);
        ESP_LOGI(TAG, "LLM pipeline initialized successfully");
    }

    LLMPipeline::~LLMPipeline() {
        shutdown();
    }

    void LLMPipeline::shutdown() {
        ESP_LOGI(TAG, "Shutting down LLM pipeline");

        stop();
        cleanupResponsePipeline();

        if (audioDataQueue_) {
            // Clear any remaining queue items
            AudioDataItem *item;
            while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
                delete item;
            }
            vQueueDelete(audioDataQueue_);
            audioDataQueue_ = nullptr;
        }

        getState(State::IDLE);
    }

    esp_err_t LLMPipeline::queueAudioChunk(const uint8_t *audio_data, size_t length, bool is_final) {
        if (state_.load() == State::IDLE) {
            ESP_LOGI(TAG, "Starting LLM audio playback");
            getState(State::PREPARING);

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
                getState(State::ERROR);
                return ESP_FAIL;
            }

            getState(State::PLAYING);
        }

        // Create audio data item
        auto *item = new AudioDataItem();
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

    esp_err_t LLMPipeline::stop() {
        ESP_LOGI(TAG, "Stopping LLM audio playback");

        getState(State::IDLE);

        // Stop playback task
        if (playbackTaskHandle_) {
            vTaskDelete(playbackTaskHandle_);
            playbackTaskHandle_ = nullptr;
        }

        // Clear audio queue
        if (audioDataQueue_) {
            AudioDataItem *item;
            while (xQueueReceive(audioDataQueue_, &item, 0) == pdTRUE) {
                delete item;
            }
        }

        audio_pipeline_stop(responsePipeline_.get());

        return ESP_OK;
    }

    esp_err_t LLMPipeline::pause() {
        if (state_.load() == State::PLAYING) {
            getState(State::PAUSED);
            ESP_LOGI(TAG, "LLM audio playback paused");
        }
        return ESP_OK;
    }

    esp_err_t LLMPipeline::resume() {
        if (state_.load() == State::PAUSED) {
            getState(State::PLAYING);
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

        // Start response pipeline
        if (responsePipeline_) {
            esp_err_t ret = audio_pipeline_run(responsePipeline_.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start response pipeline: %s", esp_err_to_name(ret));
                getState(State::ERROR);
                return;
            }
        }

        bool playback_complete = false;

        while (state_.load() != State::IDLE && !playback_complete) {
            AudioDataItem *item = nullptr;

            // Wait for audio data
            if (xQueueReceive(audioDataQueue_, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (item && state_.load() == State::PLAYING) {
                    esp_err_t ret = writeAudioToPipeline(item->data.data(), item->data.size());
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to write audio to pipeline: %s", esp_err_to_name(ret));
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

        getState(State::IDLE);
        ESP_LOGI(TAG, "LLM audio playback task ended");
    }

    esp_err_t LLMPipeline::setupResponsePipeline() {
        ESP_LOGI(TAG, "Setting up LLM response audio pipeline");

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

        auto audioPipeline = AudioPipelineUtil::createAudioPipeline(
                AudioSource::VOICE_RESPONSE,
                raw_handle,
                false,
                DEFAULT_SAMPLE_RATE,
                DEFAULT_CHANNELS,
                DEFAULT_BITS
        );

        responsePipeline_ = std::move(audioPipeline->pipeline);
        rawResponseStream_ = std::move(audioPipeline->i2SStreamReader);
        i2sResponseWriter_ = std::move(audioPipeline->i2SStreamWriter);
        responseEvent_ = std::move(audioPipeline->evtHandle);

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

    esp_err_t LLMPipeline::writeAudioToPipeline(const uint8_t *data, size_t length) {
        if (!rawResponseStream_ || !data || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        int bytes_written = raw_stream_write(rawResponseStream_.get(), (char *) data, length);
        if (bytes_written != length) {
            ESP_LOGW(TAG, "Incomplete write: %d/%zu bytes", bytes_written, length);
            return ESP_FAIL;
        }

        return ESP_OK;
    }

    void LLMPipeline::getState(State new_state) {
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        }
    }

} // namespace btspeaker 