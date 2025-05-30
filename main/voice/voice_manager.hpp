//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_VOICE_MANAGER_H
#define BT_SPEAKER_VOICE_MANAGER_H

#pragma once

#include "../audio/audio_types.hpp"
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

// Voice processing modes
enum class VoiceProcessingMode : uint8_t {
    AudioToAudio,     // Direct audio→LLM→audio (recommended, network-based)
    TextBased,        // STT→LLM→TTS (traditional, network-based)
    LocalDetection,   // Only local wake word + voice activity detection
    Disabled          // Voice features off
};

// Voice command types for routing
enum class VoiceCommandType : uint8_t {
    Unknown,
    MusicControl,     // "Play", "Stop", "Next", "Volume"
    MusicQuery,       // "What's playing?", "Who is this?"
    MusicRequest,     // "Play jazz", "Play workout music"
    GeneralQuery,     // "What's the weather?", "Tell me a joke"
    SystemControl,    // "Turn off", "Connect WiFi"
    Conversation      // Natural chat
};

// Voice processing configuration
struct VoiceConfig {
    VoiceProcessingMode mode = VoiceProcessingMode::AudioToAudio;
    
    // Wake word detection (LOCAL - can run on ESP32)
    bool wake_word_enabled = true;
    std::string wake_word = "Hey Speaker";
    float wake_word_threshold = 0.7f;
    bool use_local_wake_word = true;  // Use tiny local model vs cloud detection
    
    // Voice activity detection (LOCAL - can run on ESP32)  
    bool voice_activity_detection = true;
    float voice_threshold = 0.02f;
    uint32_t silence_timeout_ms = 2000;
    
    // Audio processing
    uint32_t sample_rate = 16000;  // 16kHz for voice
    uint32_t buffer_duration_ms = 3000;  // 3 second voice buffer
    
    // NETWORK-BASED LLM API settings (ALL AI processing is cloud-based)
    std::string llm_endpoint = "https://api.openai.com/v1/realtime";
    std::string api_key = "";  // Set via configuration
    uint32_t response_timeout_ms = 5000;
    uint32_t max_retries = 3;
    
    // Network requirements
    bool require_internet = true;  // All AI features need internet
    uint32_t min_bandwidth_kbps = 64;  // Minimum for voice streaming
    
    bool is_valid() const {
        if (mode == VoiceProcessingMode::LocalDetection) {
            return sample_rate > 0;  // Local mode doesn't need API key
        }
        return !api_key.empty() && sample_rate > 0 && 
               buffer_duration_ms > 0 && response_timeout_ms > 0;
    }
    
    bool needs_internet() const {
        return mode == VoiceProcessingMode::AudioToAudio || 
               mode == VoiceProcessingMode::TextBased;
    }
};

// Voice command result
struct VoiceCommandResult {
    VoiceCommandType type = VoiceCommandType::Unknown;
    std::string command_text = "";
    std::string response_text = "";
    float confidence = 0.0f;
    
    // Audio response (for audio-to-audio mode)
    std::vector<uint8_t> audio_response;
    uint32_t audio_sample_rate = 16000;
    
    // Action parameters (for music/system control)
    std::unordered_map<std::string, std::string> parameters;
    
    bool has_audio_response() const { return !audio_response.empty(); }
    bool has_text_response() const { return !response_text.empty(); }
    bool has_action() const { return type != VoiceCommandType::Unknown && type != VoiceCommandType::Conversation; }
};

// Voice processing callbacks
using VoiceCommandCallback = std::function<void(const VoiceCommandResult&)>;
using VoiceErrorCallback = std::function<void(const std::string&)>;

// Advanced Voice Processing Manager
class VoiceManager {
private:
    VoiceConfig config_;
    std::atomic<bool> initialized_;
    std::atomic<bool> listening_;
    std::atomic<bool> processing_;
    
    // Audio buffers
    std::unique_ptr<VoiceFrameBuffer> voice_buffer_;
    std::unique_ptr<SampleBuffer> audio_input_buffer_;
    
    // Processing tasks
    std::unique_ptr<Task> voice_detection_task_;
    std::unique_ptr<Task> voice_processing_task_;
    
    // Callbacks
    VoiceCommandCallback command_callback_;
    VoiceErrorCallback error_callback_;
    
    // Performance metrics
    mutable std::atomic<uint32_t> commands_processed_;
    mutable std::atomic<uint32_t> processing_errors_;
    mutable std::atomic<uint32_t> avg_response_time_ms_;
    
    static constexpr const char* TAG = "VOICE_MANAGER";
    
public:
    explicit VoiceManager(const VoiceConfig& config);
    ~VoiceManager();
    
    // Non-copyable, non-movable
    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;
    
    // Lifecycle management
    bool initialize();
    bool start_listening();
    void stop_listening();
    void shutdown();
    
    // Configuration
    void set_config(const VoiceConfig& config);
    VoiceConfig get_config() const { return config_; }
    void set_wake_word(const std::string& wake_word);
    void set_processing_mode(VoiceProcessingMode mode);
    
    // Callback registration
    void set_command_callback(VoiceCommandCallback callback) { command_callback_ = std::move(callback); }
    void set_error_callback(VoiceErrorCallback callback) { error_callback_ = std::move(callback); }
    
    // Audio input interface
    bool process_audio_input(const VoiceFrame& frame);
    bool process_raw_audio(const int16_t* samples, size_t count);
    
    // Manual voice processing (for testing)
    VoiceCommandResult process_voice_command(const std::vector<uint8_t>& audio_data);
    VoiceCommandResult process_text_command(const std::string& text);
    
    // Status and monitoring
    bool is_listening() const { return listening_.load(std::memory_order_acquire); }
    bool is_processing() const { return processing_.load(std::memory_order_acquire); }
    bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }
    
    // Performance metrics
    struct VoiceMetrics {
        uint32_t commands_processed = 0;
        uint32_t processing_errors = 0;
        uint32_t avg_response_time_ms = 0;
        float voice_activity_ratio = 0.0f;
        uint32_t wake_word_detections = 0;
    };
    
    VoiceMetrics get_metrics() const;
    void reset_metrics();
    void print_status() const;
    
private:
    // Core processing methods
    void voice_detection_loop();
    void voice_processing_loop();
    
    // Voice activity detection
    bool detect_voice_activity(const VoiceFrame& frame);
    bool detect_wake_word(const std::vector<uint8_t>& audio_data);
    
    // LLM processing modes (ALL NETWORK-BASED)
    VoiceCommandResult process_audio_to_audio_cloud(const std::vector<uint8_t>& audio_data);
    VoiceCommandResult process_text_based_cloud(const std::vector<uint8_t>& audio_data);
    VoiceCommandResult process_local_detection_only(const std::vector<uint8_t>& audio_data);
    
    // Command classification and routing (NETWORK-BASED for sophisticated AI)
    VoiceCommandType classify_command_cloud(const std::string& text);
    VoiceCommandType classify_command_local_basic(const std::string& text);  // Simple keyword matching
    void extract_command_parameters(const std::string& text, VoiceCommandResult& result);
    
    // API communication (NETWORK REQUIRED)
    bool send_audio_to_llm(const std::vector<uint8_t>& audio_data, VoiceCommandResult& result);
    bool send_text_to_llm(const std::string& text, VoiceCommandResult& result);
    bool check_network_connectivity();
    
    // Audio processing utilities
    std::vector<uint8_t> record_voice_command();
    bool convert_audio_format(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);
    
    // Local-only processing (CAN run on ESP32)
    bool detect_wake_word_local(const std::vector<uint8_t>& audio_data);  // Tiny model
    bool detect_voice_activity_local(const VoiceFrame& frame);            // Simple energy detection
    VoiceCommandType classify_simple_commands_local(const std::string& text);  // Basic keyword matching
    
    // Error handling
    void handle_processing_error(const std::string& error);
    
    // Utility methods
    const char* processing_mode_to_string(VoiceProcessingMode mode) const;
    const char* command_type_to_string(VoiceCommandType type) const;
};

// Factory for creating voice manager
namespace VoiceManagerFactory {
    // Create voice manager with audio-to-audio LLM (REQUIRES INTERNET)
    std::unique_ptr<VoiceManager> create_audio_to_audio_cloud(const std::string& api_key);
    
    // Create voice manager with traditional text-based pipeline (REQUIRES INTERNET)
    std::unique_ptr<VoiceManager> create_text_based_cloud(const std::string& api_key);
    
    // Create voice manager with local detection only (NO INTERNET REQUIRED)
    // Only provides wake word detection and voice activity detection
    std::unique_ptr<VoiceManager> create_local_detection_only();
    
    // Create voice manager with automatic fallback: cloud when available, local when offline
    std::unique_ptr<VoiceManager> create_adaptive_manager(const std::string& api_key);
}

} // namespace smart_speaker

#endif //BT_SPEAKER_VOICE_MANAGER_H 