#include "../include/esp_raii_wrapper.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

static constexpr const char* TAG = "ESP_HANDLE";

void audioPipelineDeleter(audio_pipeline_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio pipeline");
        // Stop pipeline before terminating to avoid NULL pointer issues
        audio_pipeline_stop(handle);
        audio_pipeline_wait_for_stop(handle);
        audio_pipeline_terminate(handle);
        audio_pipeline_deinit(handle);
    }
}

void audioElementDeleter(audio_element_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio element");
        // Stop element if it's running
        audio_element_state_t state = audio_element_get_state(handle);
        if (state == AEL_STATE_RUNNING || state == AEL_STATE_PAUSED) {
            audio_element_stop(handle);
        }
        audio_element_deinit(handle);
    }
}

void audioEventIfaceDeleter(audio_event_iface_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio event interface");
        audio_event_iface_destroy(handle);
    }
}

} // namespace btspeaker 