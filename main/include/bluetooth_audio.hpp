#pragma once

#include "bluetooth_initializer.hpp"
#include "avrcp_handler.hpp"
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

class BluetoothAudioApp {
public:
    enum class State {
        UNINITIALIZED,
        INITIALIZED, 
        RUNNING,
        STOPPED,
        ERROR
    };

    BluetoothAudioApp();
    ~BluetoothAudioApp();
    
    BluetoothAudioApp(const BluetoothAudioApp&) = delete;
    BluetoothAudioApp& operator=(const BluetoothAudioApp&) = delete;

    esp_err_t init();
    esp_err_t run();
    void shutdown();
    
    State get_state() const;

private:
    static constexpr const char* TAG = "BT_AUDIO_APP";
    
    // Music playback command callback for voice assistant
    void music_playback_cmd_callback(const char* command);
    
    std::atomic<State> state_;
    static BluetoothAudioApp* instance_;
    
    std::unique_ptr<WiFiManager> wifi_manager_;
    std::unique_ptr<BluetoothInitializer> initializer_;
    std::unique_ptr<AvrcpHandler> avrcp_handler_;
    std::unique_ptr<AudioPipelineManager> pipeline_manager_;
    std::unique_ptr<VoiceAssistantManager> voice_assistant_;
};

} // namespace btspeaker 