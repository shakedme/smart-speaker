#include "playback_app_manager.hpp"
#include <memory>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

static constexpr const char *TAG = "PLAYBACK_MAIN";
static std::unique_ptr<btspeaker::PlaybackAppManager> g_app;

extern "C" void app_main() {
    ESP_LOGI(TAG, "Initializing playback_node firmware");
    g_app = std::make_unique<btspeaker::PlaybackAppManager>();
    const esp_err_t ret = g_app->start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback_node: %s", esp_err_to_name(ret));
        g_app.reset();
        return;
    }

    while (true) {
        ESP_LOGI(TAG,
                 "DRAM=%lu PSRAM=%lu",
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
