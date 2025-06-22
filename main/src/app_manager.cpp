#include "../include/app_manager.hpp"
#include "../include/voice_config.hpp"
#include "../include/audio_source_manager.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {


    AppManager::AppManager() {
        initNVS();

        wifiManager = std::make_unique<WiFiManager>();
        wifiManager->connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

        audioSourceManager = std::make_unique<AudioSourceManager>(*wifiManager);

        if (wifiManager->isConnected()) {
            voiceAssistant = std::make_unique<VoiceAssistantManager>(*audioSourceManager);
            voiceAssistant->setMusicPlaybackCallback(
                    [this](const char *command) {
                        this->musicPlaybackCmdCallback(command);
                    }
            );
        } else {
            ESP_LOGW(TAG, "WiFi not connected, voice assistant disabled");
        }

        ESP_LOGI(TAG, "Application initialized successfully");
    }

    AppManager::~AppManager() {
        shutdown();
    }

    esp_err_t AppManager::run() {
        ESP_LOGI(TAG, "Starting application");

        audioSourceManager->start();

        if (voiceAssistant) {
            voiceAssistant->start();
        }

        ESP_LOGI(TAG, "Application running. Starting event loop...");

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // Simple event loop for now
        }
    }

    void AppManager::shutdown() {
        // Stop voice assistant first
        if (voiceAssistant) {
            voiceAssistant->stop();
            voiceAssistant->shutdown();
            voiceAssistant.reset();
        }

        // Clean up AudioSourceManager
        audioSourceManager->stop();

        // Clean up WiFi
        if (wifiManager) {
            wifiManager->shutdown();
            wifiManager.reset();
        }

        ESP_LOGI(TAG, "Application shutdown complete");
    }


    esp_err_t AppManager::initNVS() {
        ESP_LOGD(TAG, "Initializing NVS");
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS needs erase and reinit: %s", esp_err_to_name(ret));
            ret = nvs_flash_erase();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
                return ret;
            }
            ret = nvs_flash_init();
        }

        return ret;
    }

    void AppManager::musicPlaybackCmdCallback(const char *command) {
        if (!command) {
            ESP_LOGW(TAG, "Invalid music command");
            return;
        }

        ESP_LOGI(TAG, "Processing voice music command: %s", command);
        audioSourceManager->onMusicCommand(command);
    }

} // namespace btspeaker 