#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <atomic>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

// Forward declarations
class GeminiApiClient;
class AudioResponsePlayer;

// Callback type for music playback commands
using MusicPlaybackCallback = std::function<void(const char* command)>;

/**
 * @brief Processes voice commands and handles business logic
 * 
 * Responsible for:
 * - Audio recording and buffering
 * - Music command detection and routing
 * - Gemini API integration for general queries
 * - Audio response playback coordination
 */
class VoiceCommandProcessor {
public:
    enum class State {
        IDLE,
        RECORDING,
        PROCESSING,
        PLAYING_RESPONSE,
        ERROR
    };

    VoiceCommandProcessor();
    ~VoiceCommandProcessor();
    
    // Delete copy operations
    VoiceCommandProcessor(const VoiceCommandProcessor&) = delete;
    VoiceCommandProcessor& operator=(const VoiceCommandProcessor&) = delete;
    
    /**
     * @brief Initialize voice command processor
     */
    esp_err_t init(GeminiApiClient* gemini_client, AudioResponsePlayer* response_player);
    
    /**
     * @brief Shutdown voice command processor
     */
    void shutdown();
    
    /**
     * @brief Start recording a voice command
     */
    esp_err_t start_recording();
    
    /**
     * @brief Process audio data during recording
     */
    esp_err_t process_audio_data(const uint8_t* data, size_t length);
    
    /**
     * @brief Finish recording and process the command
     */
    esp_err_t finish_recording();
    
    /**
     * @brief Cancel current recording
     */
    esp_err_t cancel_recording();
    
    /**
     * @brief Set callback for music playback commands
     */
    void set_music_playback_callback(MusicPlaybackCallback callback);
    
    /**
     * @brief Get current processing state
     */
    State get_state() const { return state_.load(); }
    
    /**
     * @brief Check if currently recording
     */
    bool is_recording() const { return state_.load() == State::RECORDING; }

private:
    static constexpr const char* TAG = "VOICE_CMD";
    static constexpr int MAX_RECORDING_DURATION_MS = 10000; // 10 seconds
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int BITS_PER_SAMPLE = 16;
    
    // Command processing
    bool check_music_command(const char* text);
    esp_err_t send_to_gemini(const uint8_t* audio_data, size_t length);
    
    // Audio response handling
    void handle_gemini_response(const uint8_t* audio_data, size_t length, bool is_final);
    
    // State management
    void set_state(State new_state);
    
    // Member variables
    std::atomic<State> state_;
    SemaphoreHandle_t state_mutex_;
    
    // Recording buffer
    std::unique_ptr<uint8_t[]> recording_buffer_;
    size_t recording_buffer_pos_;
    size_t max_buffer_size_;
    
    // External components
    GeminiApiClient* gemini_client_;
    AudioResponsePlayer* response_player_;
    MusicPlaybackCallback music_callback_;
    
    // Music command keywords
    static const char* MUSIC_COMMANDS[];
    static const size_t MUSIC_COMMANDS_COUNT;
};

} // namespace btspeaker 