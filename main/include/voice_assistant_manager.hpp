#pragma once

#include "esp_raii_wrapper.hpp"
#include "bluetooth_pipeline.hpp"
#include <memory>
#include <atomic>
#include <functional>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

// Forward declarations
class MicrophoneManager;
class WakeWordDetector;
class VoiceCommandProcessor;
class WebSocketVoiceClient;
class LLMPipeline;

// Callback type for music playback commands
using MusicPlaybackCallback = std::function<void(const char* command)>;

/**
 * @brief High-level orchestration of voice assistant components
 * 
 * Simplified responsibilities:
 * - Initialize and coordinate sub-components
 * - Handle overall state management and flow
 * - Provide high-level API for voice assistant functionality
 * - Route audio data between components
 * - Manage WebSocket connection for streaming voice processing
 */
class VoiceAssistantManager {
public:
    enum class State {
        UNINITIALIZED,
        LISTENING,
        WAKE_WORD_DETECTED,
        RECORDING_COMMAND,
        PROCESSING_COMMAND,
        PLAYING_RESPONSE,
        ERROR
    };

    VoiceAssistantManager();
    ~VoiceAssistantManager();
    
    // Delete copy operations
    VoiceAssistantManager(const VoiceAssistantManager&) = delete;
    VoiceAssistantManager& operator=(const VoiceAssistantManager&) = delete;
    
    /**
     * @brief Initialize voice assistant with main audio pipeline
     */
    esp_err_t init(BluetoothPipeline* main_pipeline_manager);
    
    /**
     * @brief Start voice assistant
     */
    esp_err_t start();
    
    /**
     * @brief Stop voice assistant
     */
    esp_err_t stop();
    
    /**
     * @brief Shutdown voice assistant
     */
    void shutdown();
    
    /**
     * @brief Set callback for music playback commands
     */
    void set_music_playback_callback(MusicPlaybackCallback callback);
    
    /**
     * @brief Get current state
     */
    State get_state() const { return state_.load(); }
    
    /**
     * @brief Manually trigger voice command recording (for testing or button trigger)
     */
    esp_err_t trigger_voice_command();

private:
    static constexpr const char* TAG = "VOICE_ASST";
    
    // Component coordination callbacks
    void on_audio_data(const uint8_t* data, size_t length);
    void on_wake_word_detected();
    
    // State management
    void set_state(State new_state);
    
    // Member variables
    std::atomic<State> state_;
    SemaphoreHandle_t state_mutex_;
    
    // Sub-components (using composition instead of inheritance)
    std::unique_ptr<MicrophoneManager> microphone_manager_;
    std::unique_ptr<WakeWordDetector> wake_word_detector_;
    std::unique_ptr<VoiceCommandProcessor> command_processor_;
    std::unique_ptr<WebSocketVoiceClient> websocket_client_;
    std::unique_ptr<LLMPipeline> response_player_;
    
    // External references
    BluetoothPipeline* main_pipeline_manager_;
    MusicPlaybackCallback music_callback_;
};

} // namespace btspeaker 