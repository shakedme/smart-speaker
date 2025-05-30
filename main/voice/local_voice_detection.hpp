//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_LOCAL_VOICE_DETECTION_H
#define BT_SPEAKER_LOCAL_VOICE_DETECTION_H

#pragma once

#include "../audio/audio_types.hpp"
#include <atomic>
#include <array>

extern "C" {
#include "esp_log.h"
#include "esp_dsp.h"  // ESP-DSP library for FFT and signal processing
}

namespace smart_speaker {

// Local voice detection capabilities (CAN run on ESP32)
class LocalVoiceDetection {
private:
    static constexpr const char* TAG = "LOCAL_VOICE";
    
    // Tiny models that fit in ESP32 memory
    static constexpr size_t WAKE_WORD_MODEL_SIZE = 100 * 1024;  // 100KB max
    static constexpr size_t VAD_MODEL_SIZE = 20 * 1024;         // 20KB max
    
    // Audio processing parameters
    static constexpr uint32_t SAMPLE_RATE = 16000;
    static constexpr uint32_t FRAME_SIZE = 320;  // 20ms at 16kHz
    static constexpr uint32_t FFT_SIZE = 512;
    
    // Simple energy-based VAD
    struct VADState {
        float energy_threshold = 0.02f;
        float energy_history[10] = {0};
        uint8_t history_index = 0;
        bool voice_detected = false;
    };
    
    // Simple wake word detection using audio fingerprinting
    struct WakeWordState {
        std::array<float, 32> target_fingerprint = {0};  // Audio fingerprint of wake word
        float similarity_threshold = 0.7f;
        uint32_t detection_count = 0;
    };
    
    VADState vad_state_;
    WakeWordState wake_word_state_;
    std::atomic<bool> initialized_;
    
public:
    LocalVoiceDetection();
    ~LocalVoiceDetection() = default;
    
    // Initialization
    bool initialize(const std::string& wake_word = "Hey Speaker");
    bool load_wake_word_model(const std::string& wake_word);
    
    // Voice Activity Detection (energy-based, ~5KB code)
    bool detect_voice_activity(const VoiceFrame& frame);
    bool detect_voice_activity(const int16_t* samples, size_t count);
    
    // Wake Word Detection (simple fingerprint matching, ~10KB code)
    bool detect_wake_word(const VoiceFrame& frame);
    bool detect_wake_word(const int16_t* samples, size_t count);
    
    // Audio feature extraction
    std::array<float, 32> extract_audio_fingerprint(const int16_t* samples, size_t count);
    float calculate_energy(const int16_t* samples, size_t count);
    float calculate_zero_crossing_rate(const int16_t* samples, size_t count);
    
    // Simple command detection (basic keyword spotting)
    enum class SimpleCommand : uint8_t {
        Unknown, Play, Pause, Stop, Next, Previous, VolumeUp, VolumeDown
    };
    
    SimpleCommand detect_simple_command(const int16_t* samples, size_t count);
    
    // Configuration
    void set_vad_threshold(float threshold) { vad_state_.energy_threshold = threshold; }
    void set_wake_word_threshold(float threshold) { wake_word_state_.similarity_threshold = threshold; }
    
    // Status
    bool is_voice_detected() const { return vad_state_.voice_detected; }
    float get_current_energy() const;
    uint32_t get_wake_word_detections() const { return wake_word_state_.detection_count; }
    
    // Memory usage info
    static constexpr size_t get_max_memory_usage() { 
        return WAKE_WORD_MODEL_SIZE + VAD_MODEL_SIZE + sizeof(LocalVoiceDetection); 
    }
    
private:
    // Signal processing helpers (using ESP-DSP)
    void apply_window(float* samples, size_t count);
    void compute_fft(const float* input, float* output, size_t size);
    float compute_spectral_centroid(const float* fft_magnitude, size_t size);
    
    // Wake word training/fingerprinting
    void train_wake_word_fingerprint(const std::vector<VoiceFrame>& training_samples);
    float compare_fingerprints(const std::array<float, 32>& fp1, const std::array<float, 32>& fp2);
    
    // Utility methods
    const char* simple_command_to_string(SimpleCommand cmd) const;
};

// Factory for local voice detection
namespace LocalVoiceDetectionFactory {
    // Create local voice detection with default wake word
    std::unique_ptr<LocalVoiceDetection> create_default();
    
    // Create with custom wake word (requires training samples)
    std::unique_ptr<LocalVoiceDetection> create_with_wake_word(const std::string& wake_word);
    
    // Memory usage estimation
    size_t estimate_memory_usage();
}

} // namespace smart_speaker

#endif //BT_SPEAKER_LOCAL_VOICE_DETECTION_H 