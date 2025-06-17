#pragma once

#include "voice_assistant_manager.hpp"
#include "wifi_manager.hpp"
#include <memory>
#include <atomic>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
}

namespace btspeaker {

class AppManager {
public:
    enum class State {
        UNINITIALIZED,
        INITIALIZED, 
        RUNNING,
        STOPPED,
        ERROR
    };

    AppManager();
    ~AppManager();
    
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    esp_err_t init();
    esp_err_t run();
    /**
     * @brief Shutdown the application
     */
    void shutdown();
    
    /**
     * @brief Get current application state
     */
    [[nodiscard]] State getState() const;

private:
    static constexpr const char* TAG = "BT_AUDIO_APP";
    
    // Music playback command callback for voice assistant
    void musicPlaybackCmdCallback(const char* command);

    esp_err_t initNVS();

    std::atomic<State> state_;
    static AppManager* instance_;
    
    std::unique_ptr<WiFiManager> wifiManager;
    std::unique_ptr<VoiceAssistantManager> voiceAssistant;
};

} // namespace btspeaker 