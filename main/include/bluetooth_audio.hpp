#pragma once

#include "bluetooth_initializer.hpp"
#include "avrcp_handler.hpp"
#include "audio_pipeline_manager.hpp"
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
    
    // Delete copy operations to prevent multiple instances
    BluetoothAudioApp(const BluetoothAudioApp&) = delete;
    BluetoothAudioApp& operator=(const BluetoothAudioApp&) = delete;

    esp_err_t init();
    esp_err_t run();
    void shutdown();
    
    State get_state() const;

private:
    static constexpr const char* TAG = "BT_AUDIO_APP";
    
    // Simple atomic state - no mutex needed
    std::atomic<State> state_;
    
    // Simple static instance - no mutex needed for single-threaded use
    static BluetoothAudioApp* instance_;
    
    // Component managers (using composition)
    std::unique_ptr<BluetoothInitializer> initializer_;
    std::unique_ptr<AvrcpHandler> avrcp_handler_;
    std::unique_ptr<AudioPipelineManager> pipeline_manager_;
};

} // namespace btspeaker 