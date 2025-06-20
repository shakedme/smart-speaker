//
// Created by Shaked Melman on 17/06/2025.
//


#include "../include/adf_audio_sink.hpp"
#include "../include/audio_pipeline_util.hpp"
#include <stdexcept>

extern "C" {
#include "audio_common.h"
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

        // Write PCM data to raw stream
        if (audioPipeline && audioPipeline->i2SStreamReader) {
            int bytes_written = raw_stream_write(audioPipeline->i2SStreamReader.get(), (char *) buffer, bytes);
            if (bytes_written != bytes) {
                ESP_LOGW(TAG, "Partial write: %d/%zu bytes", bytes_written, bytes);
            } else {
                ESP_LOGD(TAG, "Successfully wrote %d bytes to audio pipeline", bytes_written);
            }
        } else {
            ESP_LOGE(TAG, "Audio pipeline or i2SStreamReader is null!");
        }
    }

    void ADFAudioSink::setupPipeline() {
        ESP_LOGI(TAG, "Setting up ADF audio pipeline for Spotify");
        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = 8 * 1024,  // 8KB buffer for Spotify audio
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
            ESP_LOGI(TAG, "ADF audio pipeline stopped");
        }
    }

    void ADFAudioSink::resume() {
        if (audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_resume(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Pipeline resumed");
            }
        }
    }

    void ADFAudioSink::pause() {
        if (audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_pause(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Pipeline paused");
            }
        }
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
