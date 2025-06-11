//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"
#include "include/app_manager.hpp"
#include <memory>
#include "CSpotContext.h"
#include "PCM5102AudioSink.h"

extern "C" {
#include "esp_log.h"
}

static constexpr const char *TAG = "MAIN";
static std::unique_ptr <btspeaker::AppManager> g_app_instance;

extern "C" {
// Declare cspot init function
void init_cspot() {
    ESP_LOGI("MAIN", "Initializing CSpot...");

    // This will verify that cspot compiles and links correctly
    auto audioSink = std::make_shared<PCM5102AudioSink>();
    ESP_LOGI("MAIN", "CSpot audio sink created");
}


void init() {
    if (!g_app_instance) {
        g_app_instance = std::make_unique<btspeaker::AppManager>();
    }

    esp_err_t ret = g_app_instance->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize app: %s", esp_err_to_name(ret));
    }

    init_cspot();
}


void run() {
    if (!g_app_instance) {
        ESP_LOGE(TAG, "App not initialized, call init() first");
        return;
    }

    esp_err_t ret = g_app_instance->run();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Run() failed: %s", esp_err_to_name(ret));
    }
}

void shutdown() {
    if (g_app_instance) {
        g_app_instance->shutdown();
        g_app_instance.reset();
    }
}

void app_main() {
    // Initialize the application
    init();

    // Run the main application (this will block)
    run();

    // Clean shutdown (this line may never be reached in normal operation)
    shutdown();
}

}