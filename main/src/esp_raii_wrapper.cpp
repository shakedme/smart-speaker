#include "../include/esp_raii_wrapper.hpp"

namespace btspeaker {

    static constexpr const char *TAG = "ESP_HANDLE";


    void audioPipelineDeleter(audio_pipeline_handle_t handle) {
        if (handle) {
            audio_pipeline_stop(handle);
            audio_pipeline_wait_for_stop(handle);
            audio_pipeline_terminate(handle);
            audio_pipeline_deinit(handle);
        }
    }


} // namespace btspeaker 