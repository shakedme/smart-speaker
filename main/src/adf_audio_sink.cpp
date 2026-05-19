//
// Created by Shaked Melman on 17/06/2025.
//


#include "../include/adf_audio_sink.hpp"
#include "../include/audio_pipeline_util.hpp"
#include <algorithm>
#include <stdexcept>

extern "C" {
#include "audio_common.h"
#include "ringbuf.h"
#include "i2s_stream.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    namespace {
        size_t ringbufFilled(ringbuf_handle_t rb) {
            return rb ? rb_bytes_filled(rb) : 0;
        }
    }

    ADFAudioSink::ADFAudioSink() :
            pipelineRunning_(false),
            initialized_(false) {
        ESP_LOGI(TAG, "ADFAudioSink created");
    }

    ADFAudioSink::~ADFAudioSink() {
        ESP_LOGI(TAG, "ADFAudioSink destructor called");
    }

    size_t ADFAudioSink::feedPCMFrames(const uint8_t *buffer, size_t bytes) {
        if (!initialized_ || !buffer || bytes == 0) {
            return 0;
        }

        size_t offset = 0;
        auto raw = audioPipeline->i2SStreamReader.get();
        uint32_t backoffMs = 1;

        while (offset < bytes) {
            size_t toWrite = std::min(kWriteChunkSize, bytes - offset);

            ringbuf_handle_t rb = audio_element_get_output_ringbuf(raw);
            size_t free_bytes = rb ? rb_bytes_available(rb) : 0;
            if (free_bytes == 0) {
                vTaskDelay(pdMS_TO_TICKS(backoffMs));
                if (backoffMs < 5) {
                    backoffMs *= 2;
                }
                continue;
            }

            if (free_bytes < toWrite) {
                toWrite = free_bytes;
            }
            backoffMs = 1;

            int written = raw_stream_write(raw, (char *) (buffer + offset), toWrite);
            if (written <= 0) {
                ESP_LOGE(TAG, "[RAW STREAM] Error writing to pipeline: %d", written);
                break;
            }

            offset += static_cast<size_t>(written);
        }

        return offset;
    }

    size_t ADFAudioSink::getBufferFillLevel() {
        if (!initialized_ || !audioPipeline) {
            return 0;
        }

        size_t filled = 0;
        if (audioPipeline->i2SStreamReader) {
            filled += ringbufFilled(
                    audio_element_get_output_ringbuf(audioPipeline->i2SStreamReader.get()));
        }
        if (audioPipeline->i2SStreamWriter) {
            filled += ringbufFilled(
                    audio_element_get_input_ringbuf(audioPipeline->i2SStreamWriter.get()));
        }
        return filled;
    }

    size_t ADFAudioSink::getBufferCapacity() {
        if (!initialized_ || !audioPipeline) {
            return 0;
        }

        size_t capacity = kRawRingBufferSize + (128 * 1024);
        return capacity;
    }

    void ADFAudioSink::setupPipeline() {
        ESP_LOGI(TAG, "Setting up ADF audio pipeline for Spotify");
        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = static_cast<int>(kRawRingBufferSize)
        };

        audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);
        if (!raw_handle) {
            ESP_LOGE(TAG, "Failed to create raw stream for Spotify");
            throw std::runtime_error("Failed to create raw stream for Spotify");
        }

        audioPipeline = AudioPipelineUtil::createAudioPipeline(
                AudioSource::SPOTIFY,
                raw_handle
        );

        initialized_ = true;
        ESP_LOGI(TAG, "ADF audio pipeline setup completed");
    }

    void ADFAudioSink::prepare() {
        if (!initialized_) {
            setupPipeline();
        }
    }

    void ADFAudioSink::start() {
        if (!initialized_) {
            setupPipeline();
        }

        if (audioPipeline && !pipelineRunning_) {
            audio_pipeline_run(audioPipeline->pipeline.get());
            pipelineRunning_ = true;
            isPlaying_.store(true);
            ESP_LOGI(TAG, "ADF audio pipeline started");
        } else if (!audioPipeline) {
            ESP_LOGE(TAG, "Audio pipeline not initialized");
        }
    }

    void ADFAudioSink::resume() {
        if (audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_resume(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
            } else {
                isPlaying_.store(true);
                ESP_LOGI(TAG, "Pipeline resumed");
            }
        }
    }

    void ADFAudioSink::pause() {
        if (audioPipeline && audioPipeline->pipeline && isPlaying_.load()) {
            esp_err_t ret = audio_pipeline_pause(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
            } else {
                isPlaying_.store(false);
                ESP_LOGI(TAG, "Pipeline paused");
            }
        }
    }

    void ADFAudioSink::flush() {
        if (!audioPipeline || !audioPipeline->i2SStreamReader) {
            ESP_LOGW(TAG, "Cannot flush - pipeline or reader not initialized");
            return;
        }

        pause();

        ESP_LOGI(TAG, "Flushing audio pipeline buffers");

        auto raw = audioPipeline->i2SStreamReader.get();
        if (raw) {
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
            rb = audio_element_get_output_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
        }

        if (audioPipeline->i2SStreamWriter) {
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(audioPipeline->i2SStreamWriter.get());
            if (rb) {
                rb_reset(rb);
            }
        }

        ESP_LOGI(TAG, "Audio pipeline buffers flushed");
    }

    void ADFAudioSink::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up ADF audio pipeline");
        if (audioPipeline) {
            AudioPipelineUtil::cleanupPipeline(*audioPipeline);
        }
        initialized_ = false;
        pipelineRunning_ = false;
        ESP_LOGI(TAG, "ADF audio pipeline cleanup complete");
    }


}
