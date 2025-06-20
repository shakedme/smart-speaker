#pragma once

#include "esp_raii_wrapper.hpp"
#include "bluetooth_pipeline.hpp"
#include "audio_source_manager.hpp"
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
    using MusicPlaybackCallback = std::function<void(const char *command)>;

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

        VoiceAssistantManager(AudioSourceManager &audioSourceManager);

        ~VoiceAssistantManager();

        // Delete copy operations
        VoiceAssistantManager(const VoiceAssistantManager &) = delete;

        VoiceAssistantManager &operator=(const VoiceAssistantManager &) = delete;

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
        void setMusicPlaybackCallback(const MusicPlaybackCallback &callback);

    private:
        static constexpr const char *TAG = "VOICE_ASST";

        // Component coordination callbacks
        void onAudioData(const uint8_t *data, size_t length);

        void onWakeWordDetected();

        // State management
        void setState(State new_state);

        // Member variables
        std::atomic<State> state;
        SemaphoreHandle_t stateMutex;
        AudioSourceManager &audioSourceManager;

        // Sub-components
        std::unique_ptr<MicrophoneManager> microphoneManager;
        std::unique_ptr<WakeWordDetector> wakeWordDetector;
        std::unique_ptr<VoiceCommandProcessor> voiceCommandProcessor;
        std::unique_ptr<WebSocketVoiceClient> websocketClient;
        // AudioSourceManager handles voice response playback

        MusicPlaybackCallback musicCallback;
    };

} // namespace btspeaker 