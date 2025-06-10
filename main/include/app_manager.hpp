#pragma once

#include "audio_pipeline_manager.hpp"
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
    State get_state() const;

    /**
     * @brief Get singleton instance
     */
    static AppManager* get_instance() { return instance_; }

private:
    static constexpr const char* TAG = "BT_AUDIO_APP";
    
    // Music playback command callback for voice assistant
    void music_playback_cmd_callback(const char* command);

    esp_err_t init_nvs();

    std::atomic<State> state_;
    static AppManager* instance_;
    
    std::unique_ptr<WiFiManager> wifi_manager_;
    std::unique_ptr<AudioPipelineManager> pipeline_manager_;
    std::unique_ptr<VoiceAssistantManager> voice_assistant_;
};

} // namespace btspeaker 