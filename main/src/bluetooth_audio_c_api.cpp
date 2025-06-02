#include "../include/bluetooth_audio.hpp"
#include <memory>

extern "C" {
#include "esp_log.h"
}

// Global app instance - simple and straightforward
static std::unique_ptr<btspeaker::BluetoothAudioApp> g_app_instance;

extern "C" {

void app_bluetooth_audio_init() {
    // Create instance if it doesn't exist
    if (!g_app_instance) {
        g_app_instance = std::make_unique<btspeaker::BluetoothAudioApp>();
    }
    
    esp_err_t ret = g_app_instance->init();
    if (ret != ESP_OK) {
        ESP_LOGE("APP_BT_C_API", "Failed to initialize Bluetooth Audio App: %s", esp_err_to_name(ret));
    }
}

void app_bluetooth_audio_run() {
    if (!g_app_instance) {
        ESP_LOGE("APP_BT_C_API", "App not initialized, call app_bluetooth_audio_init() first");
        return;
    }
    
    esp_err_t ret = g_app_instance->run();
    if (ret != ESP_OK) {
        ESP_LOGE("APP_BT_C_API", "Bluetooth Audio App run failed: %s", esp_err_to_name(ret));
    }
}

void app_bluetooth_audio_shutdown() {
    if (g_app_instance) {
        g_app_instance->shutdown();
        g_app_instance.reset();
    }
}

} 