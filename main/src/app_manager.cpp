#include "../include/app_manager.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

// Static member definition - simple and clean
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
        ESP_LOGI(TAG, "Initializing Bluetooth Audio Application");

        if (state_.load() != State::UNINITIALIZED) {
            ESP_LOGW(TAG, "Already initialized");
            return ESP_OK;
        }

        // Initialize WiFi first (needed for Gemini API)
        wifi_manager_ = std::make_unique<WiFiManager>();
        esp_err_t ret = wifi_manager_->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Connect to WiFi
        ret = wifi_manager_->connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Voice assistant features will be limited without internet");
            // Continue initialization - Bluetooth audio can still work
        }

        // Initialize Bluetooth subsystem
        initializer_ = std::make_unique<BluetoothInitializer>();
        ret = initializer_->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize Bluetooth: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Initialize audio pipeline manager
        pipeline_manager_ = std::make_unique<AudioPipelineManager>();
        ret = pipeline_manager_->setup_pipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup audio pipeline: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Initialize AVRCP handler
        avrcp_handler_ = std::make_unique<AvrcpHandler>();
        avrcp_handler_->set_pipeline_manager(pipeline_manager_.get());
        ret = avrcp_handler_->configure_target();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure AVRCP: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
        }

        // Initialize voice assistant manager (only if WiFi is connected)
        if (wifi_manager_->is_connected()) {
            voice_assistant_ = std::make_unique<VoiceAssistantManager>();
            ret = voice_assistant_->init(pipeline_manager_.get());
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

        ESP_LOGI(TAG, "Starting Bluetooth Audio Application");
        state_ = State::RUNNING;

        // Start audio pipeline
        esp_err_t ret = pipeline_manager_->start_pipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio pipeline: %s", esp_err_to_name(ret));
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

        ESP_LOGI(TAG, "Bluetooth Audio Application running. Starting event loop...");

        // Run the main event loop
        ret = pipeline_manager_->run_event_loop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Event loop error: %s", esp_err_to_name(ret));
            state_ = State::ERROR;
            return ret;
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

        // Clean up audio pipeline
        if (pipeline_manager_) {
            pipeline_manager_->stop_pipeline();
            pipeline_manager_->cleanup();
            pipeline_manager_.reset();
        }

        // Clean up AVRCP handler
        avrcp_handler_.reset();

        // Clean up Bluetooth
        if (initializer_) {
            initializer_->shutdown();
            initializer_.reset();
        }

        // Clean up WiFi
        if (wifi_manager_) {
            wifi_manager_->shutdown();
            wifi_manager_.reset();
        }

        state_ = State::UNINITIALIZED;
        ESP_LOGI(TAG, "Bluetooth Audio Application shutdown complete");
    }

    void AppManager::music_playback_cmd_callback(const char *command) {
        if (!command || !pipeline_manager_ || !avrcp_handler_) {
            ESP_LOGW(TAG, "Invalid music command or components not available");
            return;
        }

        ESP_LOGI(TAG, "Processing voice music command: %s", command);

        esp_err_t ret = ESP_OK;

        if (strstr(command, "play") || strstr(command, "resume")) {
            ret = pipeline_manager_->resume_pipeline();
            ESP_LOGI(TAG, "Voice command: Resume playback");
        } else if (strstr(command, "pause")) {
            ret = pipeline_manager_->pause_pipeline();
            ESP_LOGI(TAG, "Voice command: Pause playback");
        } else if (strstr(command, "stop")) {
            ret = pipeline_manager_->stop_pipeline();
            ESP_LOGI(TAG, "Voice command: Stop playback");
        } else if (strstr(command, "start over") || strstr(command, "restart")) {
            // Stop and start again
            pipeline_manager_->stop_pipeline();
            vTaskDelay(pdMS_TO_TICKS(100));
            ret = pipeline_manager_->start_pipeline();
            ESP_LOGI(TAG, "Voice command: Restart playback");
        } else {
            ESP_LOGW(TAG, "Unrecognized music command: %s", command);
            return;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to execute music command '%s': %s", command, esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Successfully executed music command: %s", command);
        }
    }

    AppManager::State AppManager::get_state() const {
        // Simple atomic read - no mutex needed
        return state_.load();
    }

} // namespace btspeaker 