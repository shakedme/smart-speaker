//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"
#include "include/app_manager.hpp"
#include <memory>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
}

static constexpr const char *TAG = "MAIN";
static std::unique_ptr<btspeaker::AppManager> g_app_instance;

extern "C" {

void app_main() {
    ESP_LOGI(TAG, "🚀 Initializing BT Speaker application");
    g_app_instance = std::make_unique<btspeaker::AppManager>();

    ESP_LOGI(TAG, "🎬 Starting application services");
    esp_err_t ret = g_app_instance->start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to start application: %s", esp_err_to_name(ret));
        g_app_instance.reset();
        return;
    }

    ESP_LOGI(TAG, "✅ Application running. Starting event loop...");

    // Simple event loop
    while (true) {
        float dram_mb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1000000.0f;
        float psram_mb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1000000.0f;
        ESP_LOGI(TAG, "DRAM: %.2f MB, PSRAM: %.2f MB", dram_mb, psram_mb);
        vTaskDelay(pdMS_TO_TICKS(2000));
   
    }
}

}