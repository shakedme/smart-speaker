//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_AI_CONTROLLER_H
#define BT_SPEAKER_AI_CONTROLLER_H

#pragma once

#include "../voice/voice_manager.hpp"
#include "../network/spotify_manager.hpp"
#include "../shared/event_bus.hpp"
#include "../shared/task_wrapper.hpp"
#include <memory>
#include <atomic>
#include <string>
#include <functional>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
}

namespace smart_speaker {

// AI processing modes
enum class AIMode : uint8_t {
    MusicFocused,     // Prioritize music control and discovery
    Conversational,   // General chat and Q&A
    Balanced,         // Mix of music and conversation
    MusicOnly,        // Only music commands, no general chat
    Disabled          // AI features disabled
};

// AI response types
enum class AIResponseType : uint8_t {
    AudioResponse,    // Direct audio playback
    MusicAction,      // Spotify playback control
    Information,      // Text/audio information response
    Error,            // Error message
    Acknowledgment    // Simple confirmation
};

// AI controller configuration
struct AIConfig {
    AIMode mode = AIMode::Balanced;
    
    // Voice processing settings
    VoiceConfig voice_config;
    
    // Spotify integration settings  
    SpotifyConfig spotify_config;
    
    // AI personality settings
    std::string ai_name = "Speaker";
    std::string ai_personality = "helpful and music-loving assistant";
    bool context_memory_enabled = true;
    uint32_t context_history_size = 10;  // Number of interactions to remember
    
    // Response preferences
    bool prefer_audio_responses = true;    // Use audio over text when possible
    bool music_auto_discovery = true;     // Suggest new music proactively
    float music_bias = 0.7f;              // 0.0 = general chat, 1.0 = music only
    
    bool is_valid() const {
        return voice_config.is_valid() && 
               (spotify_config.is_valid() || mode == AIMode::Disabled);
    }
};

// AI response structure
struct AIResponse {
    AIResponseType type = AIResponseType::Acknowledgment;
    std::string text_response = "";
    std::vector<uint8_t> audio_response;
    
    // Music actions
    MusicRequest music_request;
    std::string spotify_action;  // "play", "pause", "next", "volume_up", etc.
    std::unordered_map<std::string, std::string> action_parameters;
    
    // Response metadata
    float confidence = 0.0f;
    uint32_t processing_time_ms = 0;
    std::string llm_model_used = "";
    
    bool has_audio() const { return !audio_response.empty(); }
    bool has_music_action() const { return !music_request.is_empty() || !spotify_action.empty(); }
    bool has_text() const { return !text_response.empty(); }
};

// Conversation context for memory
struct ConversationContext {
    std::string user_input;
    std::string ai_response;
    uint64_t timestamp;
    VoiceCommandType command_type;
    bool music_related;
    
    // Current music context
    SpotifyTrack current_track;
    std::string current_activity;  // "working", "relaxing", etc.
};

// AI Controller - The Brain of the Smart Speaker
class AIController {
private:
    AIConfig config_;
    std::atomic<bool> initialized_;
    std::atomic<bool> active_;
    
    // Component managers
    std::unique_ptr<VoiceManager> voice_manager_;
    std::unique_ptr<SpotifyManager> spotify_manager_;
    
    // Context and memory
    std::vector<ConversationContext> conversation_history_;
    SpotifyPlaybackStatus current_music_context_;
    std::string current_user_activity_;
    
    // Background processing
    std::unique_ptr<Task> ai_processing_task_;
    std::unique_ptr<Task> context_update_task_;
    
    // Performance metrics
    mutable std::atomic<uint32_t> total_interactions_;
    mutable std::atomic<uint32_t> music_commands_;
    mutable std::atomic<uint32_t> chat_interactions_;
    mutable std::atomic<uint32_t> processing_errors_;
    mutable std::atomic<uint32_t> avg_response_time_ms_;
    
    static constexpr const char* TAG = "AI_CONTROLLER";
    
public:
    explicit AIController(const AIConfig& config);
    ~AIController();
    
    // Non-copyable, non-movable
    AIController(const AIController&) = delete;
    AIController& operator=(const AIController&) = delete;
    
    // Lifecycle management
    bool initialize();
    bool start();
    void stop();
    void shutdown();
    
    // Configuration management
    void set_config(const AIConfig& config);
    AIConfig get_config() const { return config_; }
    void set_ai_mode(AIMode mode);
    void set_ai_personality(const std::string& personality);
    
    // Voice interaction interface
    void on_voice_command(const VoiceCommandResult& voice_result);
    void on_voice_error(const std::string& error);
    
    // Manual interaction (for testing/debugging)
    AIResponse process_text_input(const std::string& text);
    AIResponse process_audio_input(const std::vector<uint8_t>& audio_data);
    
    // Music context management
    void update_music_context(const SpotifyPlaybackStatus& status);
    void set_user_activity(const std::string& activity);  // "working", "cooking", etc.
    
    // Conversation management
    void add_to_conversation_history(const std::string& user_input, 
                                   const std::string& ai_response,
                                   VoiceCommandType command_type);
    void clear_conversation_history();
    std::string get_conversation_summary() const;
    
    // Status and monitoring
    bool is_active() const { return active_.load(std::memory_order_acquire); }
    bool is_music_connected() const;
    bool is_voice_listening() const;
    
    // Performance metrics
    struct AIMetrics {
        uint32_t total_interactions = 0;
        uint32_t music_commands = 0;
        uint32_t chat_interactions = 0;
        uint32_t processing_errors = 0;
        uint32_t avg_response_time_ms = 0;
        float music_command_ratio = 0.0f;
        float ai_accuracy = 0.0f;
    };
    
    AIMetrics get_metrics() const;
    void reset_metrics();
    void print_status() const;
    
private:
    // Core AI processing
    AIResponse process_voice_command(const VoiceCommandResult& voice_result);
    AIResponse process_music_command(const VoiceCommandResult& voice_result);
    AIResponse process_general_query(const VoiceCommandResult& voice_result);
    
    // LLM interaction with context
    AIResponse query_llm_with_context(const std::string& user_input, 
                                     VoiceCommandType command_type);
    AIResponse query_audio_llm_with_context(const std::vector<uint8_t>& audio_data);
    
    // Music intelligence integration
    MusicRequest analyze_music_request(const std::string& user_input);
    bool execute_music_action(const std::string& action, 
                             const std::unordered_map<std::string, std::string>& params);
    std::string generate_music_response(const MusicRequest& request, bool success);
    
    // Context and memory management
    std::string build_context_prompt(const std::string& user_input, 
                                    VoiceCommandType command_type);
    void update_conversation_context();
    std::string get_relevant_music_context() const;
    std::string get_conversation_context() const;
    
    // Response processing
    void process_ai_response(AIResponse& response);
    void play_audio_response(const std::vector<uint8_t>& audio_data);
    void execute_spotify_action(const AIResponse& response);
    
    // Background tasks
    void ai_processing_loop();
    void context_update_loop();
    
    // Component integration
    bool setup_voice_callbacks();
    bool setup_spotify_integration();
    
    // Error handling and fallbacks
    AIResponse create_error_response(const std::string& error_message);
    AIResponse create_fallback_response(VoiceCommandType command_type);
    void handle_processing_error(const std::string& error);
    
    // Utility methods
    const char* ai_mode_to_string(AIMode mode) const;
    const char* response_type_to_string(AIResponseType type) const;
    VoiceCommandType classify_input_type(const std::string& text);
    float calculate_music_relevance(const std::string& text);
};

// Factory for creating AI controller
namespace AIControllerFactory {
    // Create AI controller with audio-to-audio LLM and Spotify
    std::unique_ptr<AIController> create_full_featured(
        const std::string& llm_api_key,
        const std::string& spotify_client_id,
        const std::string& spotify_client_secret
    );
    
    // Create AI controller with music-only mode
    std::unique_ptr<AIController> create_music_only(
        const std::string& spotify_client_id,
        const std::string& spotify_client_secret
    );
    
    // Create AI controller with conversation-only mode (no Spotify)
    std::unique_ptr<AIController> create_conversation_only(
        const std::string& llm_api_key
    );
}

} // namespace smart_speaker

#endif //BT_SPEAKER_AI_CONTROLLER_H 