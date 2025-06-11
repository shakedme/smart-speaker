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

        if (state_.load() != State::UNINITIALIZED) {
            ESP_LOGW(TAG, "Already initialized");
            return ESP_OK;
        }

        // Initialize NVS first
        esp_err_t ret = init_nvs();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        } else {
            ESP_LOGI(TAG, "NVS initialized successfully");
        }

        // Initialize WiFi first (needed for Gemini API)
        wifi_manager_ = std::make_unique<WiFiManager>();
        ret = wifi_manager_->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Voice assistant features will be limited without internet");
            // Continue initialization - Bluetooth audio can still work
        }

        // Initialize AudioSourceManager (replaces direct pipeline management)
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        ret = audio_source_mgr.initialize();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize AudioSourceManager: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Set up source change callback for debugging/logging
        audio_source_mgr.setSourceChangeCallback([](AudioSource from, AudioSource to) {
            ESP_LOGI(TAG, "Audio source changed from %d to %d", (int)from, (int)to);
        });

        // Initialize voice assistant manager (only if WiFi is connected)
        if (wifi_manager_->is_connected()) {
            voice_assistant_ = std::make_unique<VoiceAssistantManager>();
            ret = voice_assistant_->init(nullptr);  // AudioSourceManager handles audio internally
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize voice assistant: %s", esp_err_to_name(ret));
                ESP_LOGW(TAG, "Continuing without voice assistant features");
                voice_assistant_.reset();
            } else {
                // Set music playback callback for voice commands
                voice_assistant_->set_music_playback_callback(
                        [this](const char *command) {
                            this->music_playback_cmd_callback(command);
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
        if (state_.load() != State::INITIALIZED) {
            ESP_LOGE(TAG, "Application not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting application");
        state_ = State::RUNNING;

        // Start AudioSourceManager
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        esp_err_t ret = audio_source_mgr.start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start AudioSourceManager: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Start voice assistant if available
        if (voice_assistant_) {
            ret = voice_assistant_->start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start voice assistant: %s", esp_err_to_name(ret));
                ESP_LOGW(TAG, "Continuing without voice assistant");
                voice_assistant_.reset();
            } else {
                ESP_LOGI(TAG, "Voice assistant started successfully");
            }
        }

        ESP_LOGI(TAG, "Application running. Starting event loop...");

        // Run the main event loop (keep using the Bluetooth pipeline for now)
        // TODO: Consider moving event loop management to AudioSourceManager
        while (state_.load() == State::RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // Simple event loop for now
        }

        state_ = State::STOPPED;
        ESP_LOGI(TAG, "Bluetooth Audio Application stopped");
        return ESP_OK;
    }

    void AppManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down Bluetooth Audio Application");

        // Stop voice assistant first
        if (voice_assistant_) {
            voice_assistant_->stop();
            voice_assistant_->shutdown();
            voice_assistant_.reset();
        }

        // Clean up AudioSourceManager
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        audio_source_mgr.stop();

        // Clean up WiFi
        if (wifi_manager_) {
            wifi_manager_->shutdown();
            wifi_manager_.reset();
        }

        state_ = State::UNINITIALIZED;
        ESP_LOGI(TAG, "Bluetooth Audio Application shutdown complete");
    }


    esp_err_t AppManager::init_nvs() {
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

    void AppManager::music_playback_cmd_callback(const char *command) {
        if (!command) {
            ESP_LOGW(TAG, "Invalid music command");
            return;
        }

        ESP_LOGI(TAG, "Processing voice music command: %s", command);

        auto& audio_source_mgr = AudioSourceManager::getInstance();
        esp_err_t ret = ESP_OK;

        if (strstr(command, "play") || strstr(command, "resume")) {
            ret = audio_source_mgr.play();
            ESP_LOGI(TAG, "Voice command: Resume playback");
        } else if (strstr(command, "pause")) {
            ret = audio_source_mgr.pause();
            ESP_LOGI(TAG, "Voice command: Pause playback");
        } else if (strstr(command, "stop")) {
            ret = audio_source_mgr.stopPlayback();
            ESP_LOGI(TAG, "Voice command: Stop playback");
        } else if (strstr(command, "next") || strstr(command, "skip")) {
            ret = audio_source_mgr.skipNext();
            ESP_LOGI(TAG, "Voice command: Skip next");
        } else if (strstr(command, "previous") || strstr(command, "back")) {
            ret = audio_source_mgr.skipPrevious();
            ESP_LOGI(TAG, "Voice command: Skip previous");
        } else if (strstr(command, "bluetooth")) {
            ret = audio_source_mgr.switchToSource(AudioSource::BLUETOOTH);
            ESP_LOGI(TAG, "Voice command: Switch to Bluetooth");
        } else if (strstr(command, "spotify")) {
            ret = audio_source_mgr.switchToSource(AudioSource::SPOTIFY);
            ESP_LOGI(TAG, "Voice command: Switch to Spotify");
        } else {
            ESP_LOGW(TAG, "Unknown music command: %s", command);
            return;
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to execute music command: %s", esp_err_to_name(ret));
        }
    }



    AppManager::State AppManager::get_state() const {
        return state_.load();
    }

} // namespace btspeaker 