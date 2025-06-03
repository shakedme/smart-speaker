#include "../include/esp_raii_wrapper.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

static constexpr const char* TAG = "ESP_HANDLE";

void audio_pipeline_deleter(audio_pipeline_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio pipeline");
        audio_pipeline_terminate(handle);
        audio_pipeline_deinit(handle);
    }
}

void audio_element_deleter(audio_element_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio element");
        audio_element_deinit(handle);
    }
}

void audio_event_iface_deleter(audio_event_iface_handle_t handle) {
    if (handle) {
        ESP_LOGD(TAG, "Deleting audio event interface");
        audio_event_iface_destroy(handle);
    }
}

} // namespace btspeaker 