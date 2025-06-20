#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include "audio_source_manager.hpp"

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

// Forward declarations
class WebSocketVoiceClient;
class LLMPipeline;

// Callback type for music playback commands
using MusicPlaybackCallback = std::function<void(const char* command)>;

/**
 * @brief Processes voice commands and handles business logic
 * 
 * Responsible for:
 * - Audio streaming in real-time chunks
 * - Music command detection and routing (when server unavailable)
 * - WebSocket integration for streaming STT→LLM→TTS pipeline
 * - Audio response playback coordination
 */
class VoiceCommandProcessor {
public:
    enum class State {
        IDLE,
        PROCESSING,
        PLAYING_RESPONSE,
        ERROR
    };

    VoiceCommandProcessor(AudioSourceManager& audioSourceManager);
    ~VoiceCommandProcessor();
    
    // Delete copy operations
    VoiceCommandProcessor(const VoiceCommandProcessor&) = delete;
    VoiceCommandProcessor& operator=(const VoiceCommandProcessor&) = delete;
    
    /**
     * @brief Initialize voice command processor
     */
    esp_err_t init(WebSocketVoiceClient* websocket_client);
    
    /**
     * @brief Shutdown voice command processor
     */
    void shutdown();
    
    /**
     * @brief Cancel current recording
     */
    esp_err_t cancelRecording();
    
    /**
     * @brief Set callback for music playback commands
     */
    void setMusicPlaybackCallback(MusicPlaybackCallback callback);


private:
    static constexpr const char* TAG = "VOICE_CMD";
    // Audio response handling
    void handleWebsocketResponse(const uint8_t* audioData, size_t length, bool isFinal, const char* messageJson);
    void handleConnectionStateChange(int connectionState);
    void handleBackendStateChange(const std::string& backendState);
    
    // State management
    void setState(State newState);

    // Member variables
    std::atomic<State> state;
    SemaphoreHandle_t stateMutex;
    AudioSourceManager &audioSourceManager;

    // Streaming buffer (much smaller than previous implementation)
    std::unique_ptr<uint8_t[]> streamBuffer;

    // External components
    WebSocketVoiceClient* websocketClient;
    MusicPlaybackCallback musicCallback;
};

} // namespace btspeaker 