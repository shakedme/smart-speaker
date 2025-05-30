//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_MAIN_H
#define BT_SPEAKER_MAIN_H

#include "app/smart_speaker.hpp"
#include "shared/event_bus.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
}

using namespace smart_speaker;

static const char* TAG = "MAIN";

// Application configuration
static AppConfig create_default_config() {
    AppConfig config;

    // Audio settings
    config.enable_bluetooth = true;
    config.enable_voice = false;      // Start with just Bluetooth
    config.enable_spotify = false;
    config.enable_llm = false;

    // Performance settings
    config.enable_performance_monitoring = true;
    config.monitor_interval_ms = 5000;

    // System settings
    config.auto_start_bluetooth = true;
    config.default_volume = 0.8f;

    return config;
}

// Forward declaration - implementation in main.cpp
extern "C" void app_main();

#endif //BT_SPEAKER_MAIN_H