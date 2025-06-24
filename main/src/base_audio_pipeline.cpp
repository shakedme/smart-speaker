//
// Created by Shaked Melman on 18/06/2025.
//

#include "../include/base_audio_pipeline.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

    BaseAudioPipeline::BaseAudioPipeline(const char *tag) : TAG(tag) {}

    void BaseAudioPipeline::pause() {
        if (running && audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_pause(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Pipeline paused");
            }
        }
    }

    void BaseAudioPipeline::resume() {
        if (running && audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_resume(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Pipeline resumed");
            }
        }
    }

    void BaseAudioPipeline::start() {
        if (!running && audioPipeline && audioPipeline->pipeline) {
            esp_err_t ret = audio_pipeline_run(audioPipeline->pipeline.get());
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start pipeline: %s", esp_err_to_name(ret));
            } else {
                running = true;
                ESP_LOGI(TAG, "Pipeline started");
            }
        }
    }

} // namespace btspeaker
