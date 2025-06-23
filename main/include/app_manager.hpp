#pragma once

#include "voice_assistant_manager.hpp"
#include "wifi_manager.hpp"
#include "audio_source_manager.hpp"
#include <memory>
#include <atomic>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
}

namespace btspeaker {

    class AppManager {
    public:
        AppManager();

        ~AppManager() = default;

        esp_err_t start();

    private:
        static constexpr const char *TAG = "APP_MANAGER";

        std::unique_ptr<WiFiManager> wifiManager;
        std::unique_ptr<VoiceAssistantManager> voiceAssistantManager;
        std::unique_ptr<AudioSourceManager> audioSourceManager;

        void initNVS();

    };

} // namespace btspeaker 