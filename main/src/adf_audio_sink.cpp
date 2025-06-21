//
// Created by Shaked Melman on 17/06/2025.
//


#include "../include/adf_audio_sink.hpp"
#include "../include/audio_pipeline_util.hpp"
#include "BellUtils.h"
#include <stdexcept>

extern "C" {
#include "audio_common.h"
#include "ringbuf.h"
#include "i2s_stream.h"
#include "esp_log.h"
#include "driver/i2s.h"
}

namespace btspeaker {

    ADFAudioSink::ADFAudioSink() :
            pipelineRunning_(false),
            initialized_(false) {
        ESP_LOGI(TAG, "ADFAudioSink created");
    }

    ADFAudioSink::~ADFAudioSink() {
        ESP_LOGI(TAG, "ADFAudioSink destructor called");
    }

    void ADFAudioSink::feedPCMFrames(const uint8_t *buffer, size_t bytes) {
        if (!initialized_ || !buffer || bytes == 0) {
            return;
        }


        size_t offset = 0;
        const size_t chunkSize = 1024;
nce
        auto raw = audioPipeline->i2SStreamReader.get();
        while (offset < bytes) {
            size_t toWrite = std::min(chunkSize, bytes - offset);

in ADF ringbuf
            size_t free_bytes = rb_bytes_available(audio_element_get_output_ringbuf(raw));
            if (free_bytes < toWrite) {
                BELL_SLEEP_MS(5);
                continue;
            }


            int written = raw_stream_write(raw, (char *) (buffer + offset), toWrite);
            if (written < 0) {
                ESP_LOGE(TAG, "[RAW STREAM] Error writing to pipeline: %d", written);
                break; 
            }
            if ((size_t) written != toWrite) {
                ESP_LOGW(TAG, "[RAW STREAM] Partial write: %d/%zu bytes", written, toWrite);
            }

            offset += written;
        }
    }

    size_t ADFAudioSink::getBufferFillLevel() {
        if (!initialized_ || !audioPipeline || !audioPipeline->i2SStreamReader) {
            return 0;
        }
        // Get the ring buffer handle from the raw_stream reader element
        ringbuf_handle_t rb = audio_element_get_output_ringbuf(audioPipeline->i2SStreamReader.get());
        if (rb) {
            return rb_bytes_filled(rb);
        }
        return 0;
    }

    void ADFAudioSink::setupPipeline() {
        ESP_LOGI(TAG, "Setting up ADF audio pipeline for Spotify");
        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = static_cast<int>(1024 * 2048) // 2MB ring buffer
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

        ESP_LOGI(TAG, "ADF audio pipeline setup completed");
    }

    void ADFAudioSink::start() {
        if (!initialized_) {
            setupPipeline();
            initialized_ = true;
        }

        if (audioPipeline) {
            audio_pipeline_run(audioPipeline->pipeline.get());
            pipelineRunning_ = true;
            isPlaying_.store(true);
            ESP_LOGI(TAG, "ADF audio pipeline started");
        } else {
            ESP_LOGE(TAG, "Audio pipeline not initialized");
        }
    }

    void ADFAudioSink::stop() {
        if (audioPipeline && audioPipeline->pipeline) {
            audio_pipeline_stop(audioPipeline->pipeline.get());
            audio_pipeline_wait_for_stop(audioPipeline->pipeline.get());
            pipelineRunning_ = false;
            isPlaying_.store(false);
            ESP_LOGI(TAG, "Spotify (ADFSink) audio pipeline stopped");
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

        // Reset the raw stream ring buffer
        auto raw = audioPipeline->i2SStreamReader.get();
        if (raw) {
            // Get the ringbuf and reset it
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
            rb = audio_element_get_output_ringbuf(raw);
            if (rb) {
                rb_reset(rb);
            }
        }

        // Reset I2S writer buffers
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
        ESP_LOGI(TAG, "ADF audio pipeline cleanup complete");
    }


}
