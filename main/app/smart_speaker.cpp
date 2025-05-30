//
// Created by Shaked Melman on 29/05/2025.
//

//
// Created by Shaked Melman on 29/05/2025.
//

#include "smart_speaker.hpp"

extern "C" {
#include "esp_log.h"
}

namespace smart_speaker {

// Constructor
    SmartSpeaker::SmartSpeaker(const AppConfig& config)
            : current_state_(AppState::Initializing),
              initialized_(false),
              running_(false),
              config_(config) {

        ESP_LOGI("SMART_SPEAKER", "SmartSpeaker constructor called");
    }

// Destructor
    SmartSpeaker::~SmartSpeaker() {
        ESP_LOGI("SMART_SPEAKER", "SmartSpeaker destructor called");
        stop();
    }

// Initialize - minimal implementation
    AudioError SmartSpeaker::initialize() {
        ESP_LOGI("SMART_SPEAKER", "Initialize called");

        // For now, just mark as initialized
        initialized_.store(true, std::memory_order_release);
        current_state_.store(AppState::Ready, std::memory_order_release);

        return AudioError::OK;
    }

// Start - minimal implementation
    AudioError SmartSpeaker::start() {
        ESP_LOGI("SMART_SPEAKER", "Start called");

        if (!initialized_.load(std::memory_order_acquire)) {
            return AudioError::ConfigError;
        }

        running_.store(true, std::memory_order_release);
        return AudioError::OK;
    }

// Stop
    void SmartSpeaker::stop() {
        ESP_LOGI("SMART_SPEAKER", "Stop called");
        running_.store(false, std::memory_order_release);
    }

// Run - minimal implementation
    void SmartSpeaker::run() {
        ESP_LOGI("SMART_SPEAKER", "Run called - entering main loop");

        while (running_.load(std::memory_order_acquire)) {
            // Simple main loop - just log and sleep
            ESP_LOGI("SMART_SPEAKER", "Main loop iteration");

            // Sleep for 5 seconds
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        ESP_LOGI("SMART_SPEAKER", "Main loop exited");
    }

// Stub implementations for other methods
    bool SmartSpeaker::set_volume(float volume) {
        ESP_LOGI("SMART_SPEAKER", "Set volume: %.2f", volume);
        return true;
    }

    bool SmartSpeaker::set_muted(bool muted) {
        ESP_LOGI("SMART_SPEAKER", "Set muted: %s", muted ? "true" : "false");
        return true;
    }

    float SmartSpeaker::get_volume() const {
        return config_.default_volume;
    }

    bool SmartSpeaker::is_muted() const {
        return false;
    }

    bool SmartSpeaker::enable_bluetooth(bool enable) {
        ESP_LOGI("SMART_SPEAKER", "Enable bluetooth: %s", enable ? "true" : "false");
        return true;
    }

    bool SmartSpeaker::enable_voice_detection(bool enable) {
        ESP_LOGI("SMART_SPEAKER", "Enable voice detection: %s", enable ? "true" : "false");
        return true;
    }

    bool SmartSpeaker::enable_spotify(bool enable) {
        ESP_LOGI("SMART_SPEAKER", "Enable spotify: %s", enable ? "true" : "false");
        return true;
    }

    AudioMetrics SmartSpeaker::get_audio_metrics() const {
        AudioMetrics metrics;
        // Return empty metrics for now
        return metrics;
    }

    void SmartSpeaker::print_system_status() const {
        ESP_LOGI("SMART_SPEAKER", "=== System Status ===");
        ESP_LOGI("SMART_SPEAKER", "State: %d", static_cast<int>(current_state_.load()));
        ESP_LOGI("SMART_SPEAKER", "Running: %s", running_.load() ? "Yes" : "No");
        ESP_LOGI("SMART_SPEAKER", "Initialized: %s", initialized_.load() ? "Yes" : "No");
    }

} // namespace smart_speaker

namespace smart_speaker {

    std::unique_ptr<EventBus> GlobalEventBus::instance_;
    SemaphoreHandle_t GlobalEventBus::creation_mutex_ = nullptr;

    EventBus& GlobalEventBus::get_instance() {
        // Simple implementation without proper singleton thread safety for now
        if (!instance_) {
            instance_ = std::make_unique<EventBus>();
        }
        return *instance_;
    }

    void GlobalEventBus::destroy_instance() {
        instance_.reset();
        if (creation_mutex_) {
            vSemaphoreDelete(creation_mutex_);
            creation_mutex_ = nullptr;
        }
    }

} // namespace smart_speaker