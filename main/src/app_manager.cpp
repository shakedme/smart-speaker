#include <esp_sntp.h>
#include "../include/app_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/task_affinity.hpp"
#include "../include/audio_source_manager.hpp"
#include "../include/memory_utils.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    static constexpr const char *TAG = "APP_MANAGER";

    AppManager::~AppManager() = default;

    AppManager::AppManager() {
        ESP_LOGI(TAG, "🚀 Starting AppManager initialization");
        logTaskAffinityLayout();

        initNVS();

        wifiManager = std::make_unique<WiFiManager>();
        wifiManager->connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

        audioSourceManager = std::make_unique<AudioSourceManager>(*wifiManager);

        if (wifiManager->isConnected()) {
            voiceAssistantManager = std::make_unique<VoiceAssistantManager>(*audioSourceManager);
        }

        wifiManager->setStatusCallback([this](WiFiManager::Status status) {
            if (!voiceAssistantManager) {
                return;
            }
            if (status == WiFiManager::Status::CONNECTED) {
                voiceAssistantManager->onNetworkAvailable();
            } else if (status == WiFiManager::Status::DISCONNECTED ||
                       status == WiFiManager::Status::FAILED) {
                voiceAssistantManager->onNetworkLost();
            }
        });

        ESP_LOGI(TAG, "✅ AppManager initialization complete");
    }

    esp_err_t AppManager::start() {
        ESP_LOGI(TAG, "🎬 Starting application services");

        esp_err_t ret = audioSourceManager->start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio source manager: %s", esp_err_to_name(ret));
            return ret;
        }

        if (voiceAssistantManager) {
            ret = voiceAssistantManager->start();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start voice assistant: %s", esp_err_to_name(ret));
            }
        }

        ESP_LOGI(TAG, "✅ Application services started successfully");
        return ESP_OK;
    }

    void AppManager::initNVS() {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
    }

}