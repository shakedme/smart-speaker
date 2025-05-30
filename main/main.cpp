//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"

extern "C" void app_main() {
    ESP_LOGI(TAG, "=== ESP32 Smart Speaker Starting ===");
    ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // Set log levels for clean output
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("SMART_SPEAKER", ESP_LOG_DEBUG);
    esp_log_level_set("AUDIO_MIXER", ESP_LOG_DEBUG);
    esp_log_level_set("I2S_DRIVER", ESP_LOG_DEBUG);

    // Since exceptions are disabled, use simple error handling
    // Create application with default configuration
    AppConfig config = create_default_config();
    auto speaker = std::make_unique<SmartSpeaker>(config);

    // Initialize the application
    ESP_LOGI(TAG, "Initializing Smart Speaker...");
    AudioError result = speaker->initialize();
    if (result != AudioError::OK) {
        ESP_LOGE(TAG, "Failed to initialize Smart Speaker: %d", static_cast<int>(result));
        return;
    }

    // Start the application
    ESP_LOGI(TAG, "Starting Smart Speaker...");
    result = speaker->start();
    if (result != AudioError::OK) {
        ESP_LOGE(TAG, "Failed to start Smart Speaker: %d", static_cast<int>(result));
        return;
    }

    ESP_LOGI(TAG, "=== Smart Speaker Ready ===");
    ESP_LOGI(TAG, "Bluetooth: Connect your phone to 'ESP32-Speaker'");
    ESP_LOGI(TAG, "Audio: PCM5102A -> TPA3116D2 -> Speakers");
    ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());

    // Run the main application loop
    speaker->run();

    // Cleanup
    GlobalEventBus::destroy_instance();
    ESP_LOGI(TAG, "=== Smart Speaker Shutdown ===");
}