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

    AppManager *AppManager::instance_ = nullptr;

    AppManager::AppManager() : state_(State::UNINITIALIZED) {
        instance_ = this;
        ESP_LOGI(TAG, "AppManager created");
    }

    AppManager::~AppManager() {
        shutdown();
        if (instance_ == this) {
            instance_ = nullptr;
        }
    }

    esp_err_t AppManager::init() {
        ESP_LOGI(TAG, "Initializing application");

        if (getState() != State::UNINITIALIZED) {
            ESP_LOGW(TAG, "Already initialized");
            return ESP_OK;
        }

        // Initialize NVS first
        esp_err_t ret = initNVS();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        } else {
            ESP_LOGI(TAG, "NVS initialized successfully");
        }

        // Initialize WiFi first
        wifiManager = std::make_unique<WiFiManager>();
        ret = wifiManager->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Voice assistant features will be limited without internet");
            // Continue initialization - Bluetooth audio can still work
        }

        // Initialize AudioSourceManager
        auto& audioSourceManager = AudioSourceManager::getInstance();
        ret = audioSourceManager.initialize();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize AudioSourceManager: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Initialize voice assistant manager
        if (wifiManager->isConnected()) {
            voiceAssistant = std::make_unique<VoiceAssistantManager>();
            ret = voiceAssistant->init();  // AudioSourceManager handles audio internally
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize voice assistant: %s", esp_err_to_name(ret));
                ESP_LOGW(TAG, "Continuing without voice assistant features");
                voiceAssistant.reset();
            } else {
                // Set music playback callback for voice commands
                voiceAssistant->setMusicPlaybackCallback(
                        [this](const char *command) {
                            this->musicPlaybackCmdCallback(command);
                        }
                );
                ESP_LOGI(TAG, "Voice assistant initialized successfully");
            }
        } else {
            ESP_LOGW(TAG, "WiFi not connected, voice assistant disabled");
        }

        state_ = State::INITIALIZED;
        ESP_LOGI(TAG, "Bluetooth Audio Application initialized successfully");
        return ESP_OK;
    }

    esp_err_t AppManager::run() {
        if (getState() != State::INITIALIZED) {
            ESP_LOGE(TAG, "Application not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting application");
        state_ = State::RUNNING;

        // Start AudioSourceManager
        auto& audioSourceManager = AudioSourceManager::getInstance();
        esp_err_t ret = audioSourceManager.start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start AudioSourceManager: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Start voice assistant if available
        if (voiceAssistant) {
            ret = voiceAssistant->start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start voice assistant: %s", esp_err_to_name(ret));
                ESP_LOGW(TAG, "Continuing without voice assistant");
                voiceAssistant.reset();
            } else {
                ESP_LOGI(TAG, "Voice assistant started successfully");
            }
        }

        ESP_LOGI(TAG, "Application running. Starting event loop...");

        while (getState() == State::RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // Simple event loop for now
        }

        state_ = State::STOPPED;
        ESP_LOGI(TAG, "Bluetooth Audio Application stopped");
        return ESP_OK;
    }

    void AppManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down Bluetooth Audio Application");

        // Stop voice assistant first
        if (voiceAssistant) {
            voiceAssistant->stop();
            voiceAssistant->shutdown();
            voiceAssistant.reset();
        }

        // Clean up AudioSourceManager
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        audio_source_mgr.stop();

        // Clean up WiFi
        if (wifiManager) {
            wifiManager->shutdown();
            wifiManager.reset();
        }

        state_ = State::UNINITIALIZED;
        ESP_LOGI(TAG, "Bluetooth Audio Application shutdown complete");
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

        AudioSourceManager::getInstance().onMusicCommand(command);
    }

    AppManager::State AppManager::getState() const {
        return state_.load();
    }

} // namespace btspeaker 