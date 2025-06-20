//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"
#include "include/app_manager.hpp"
#include <memory>

extern "C" {
#include "esp_log.h"
}

static constexpr const char *TAG = "MAIN";
static std::unique_ptr<btspeaker::AppManager> g_app_instance;

extern "C" {

void app_main() {
    ESP_LOGI(TAG, "Initializing application");
    g_app_instance = std::make_unique<btspeaker::AppManager>();

    ESP_LOGI(TAG, "Starting application");
    g_app_instance->run();

    ESP_LOGI(TAG, "Shutting down application");
    g_app_instance->shutdown();

    g_app_instance.reset();
}

}