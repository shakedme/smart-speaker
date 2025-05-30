//
// Created by Shaked Melman on 29/05/2025.
//

#include "local_voice_detection.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" {
#include "esp_dsp.h"
}

namespace smart_speaker {

// Constructor
LocalVoiceDetection::LocalVoiceDetection() 
    : initialized_(false) {
    // Initialize VAD state
    std::memset(&vad_state_, 0, sizeof(vad_state_));
    vad_state_.energy_threshold = 0.02f;
    
    // Initialize wake word state
    std::memset(&wake_word_state_, 0, sizeof(wake_word_state_));
    wake_word_state_.similarity_threshold = 0.7f;
}

// Initialize the local voice detection
bool LocalVoiceDetection::initialize(const std::string& wake_word) {
    ESP_LOGI(TAG, "Initializing local voice detection with wake word: %s", wake_word.c_str());
    
    // Initialize ESP-DSP library
    esp_dsp_init();
    
    // Load/train wake word model (simplified for now)
    if (!load_wake_word_model(wake_word)) {
        ESP_LOGW(TAG, "Could not load wake word model, using default");
    }
    
    initialized_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Local voice detection initialized successfully");
    return true;
}

// Load wake word model (simplified fingerprint approach)
bool LocalVoiceDetection::load_wake_word_model(const std::string& wake_word) {
    ESP_LOGI(TAG, "Loading wake word model for: %s", wake_word.c_str());
    
    // For now, create a simple fingerprint pattern
    // In a real implementation, this would be trained from actual audio samples
    if (wake_word == "Hey Speaker") {
        // Simple pattern representing "Hey Speaker" audio characteristics
        wake_word_state_.target_fingerprint = {
            0.15f, 0.22f, 0.18f, 0.31f, 0.12f, 0.28f, 0.19f, 0.25f,
            0.17f, 0.33f, 0.14f, 0.27f, 0.21f, 0.16f, 0.29f, 0.13f,
            0.24f, 0.18f, 0.32f, 0.11f, 0.26f, 0.20f, 0.15f, 0.30f,
            0.23f, 0.17f, 0.28f, 0.19f, 0.25f, 0.14f, 0.31f, 0.16f
        };
    } else {
        // Generate a simple pattern based on string hash
        uint32_t hash = 0;
        for (char c : wake_word) {
            hash = hash * 31 + c;
        }
        
        for (size_t i = 0; i < wake_word_state_.target_fingerprint.size(); ++i) {
            hash = hash * 1103515245 + 12345;  // Simple LCG
            wake_word_state_.target_fingerprint[i] = (float)(hash % 1000) / 1000.0f;
        }
    }
    
    ESP_LOGI(TAG, "Wake word fingerprint loaded");
    return true;
}

// Voice Activity Detection using energy analysis
bool LocalVoiceDetection::detect_voice_activity(const VoiceFrame& frame) {
    return detect_voice_activity(frame.samples, frame.sample_count);
}

bool LocalVoiceDetection::detect_voice_activity(const int16_t* samples, size_t count) {
    if (!samples || count == 0) {
        return false;
    }
    
    // Calculate energy of the current frame
    float energy = calculate_energy(samples, count);
    
    // Update energy history
    vad_state_.energy_history[vad_state_.history_index] = energy;
    vad_state_.history_index = (vad_state_.history_index + 1) % 10;
    
    // Calculate average energy over recent history
    float avg_energy = 0.0f;
    for (int i = 0; i < 10; ++i) {
        avg_energy += vad_state_.energy_history[i];
    }
    avg_energy /= 10.0f;
    
    // Voice detected if current energy significantly above threshold and average
    bool voice_detected = (energy > vad_state_.energy_threshold) && 
                         (energy > avg_energy * 1.5f);
    
    vad_state_.voice_detected = voice_detected;
    
    if (voice_detected) {
        ESP_LOGD(TAG, "Voice activity detected (energy: %.4f, threshold: %.4f)", 
                energy, vad_state_.energy_threshold);
    }
    
    return voice_detected;
}

// Wake word detection using audio fingerprinting
bool LocalVoiceDetection::detect_wake_word(const VoiceFrame& frame) {
    return detect_wake_word(frame.samples, frame.sample_count);
}

bool LocalVoiceDetection::detect_wake_word(const int16_t* samples, size_t count) {
    if (!samples || count == 0 || !initialized_.load(std::memory_order_acquire)) {
        return false;
    }
    
    // First check if there's voice activity
    if (!detect_voice_activity(samples, count)) {
        return false;
    }
    
    // Extract audio fingerprint from current frame
    auto current_fingerprint = extract_audio_fingerprint(samples, count);
    
    // Compare with target wake word fingerprint
    float similarity = compare_fingerprints(current_fingerprint, wake_word_state_.target_fingerprint);
    
    // Wake word detected if similarity above threshold
    bool detected = similarity > wake_word_state_.similarity_threshold;
    
    if (detected) {
        wake_word_state_.detection_count++;
        ESP_LOGI(TAG, "Wake word detected! (similarity: %.3f, count: %lu)", 
                similarity, wake_word_state_.detection_count);
    }
    
    return detected;
}

// Extract simple audio fingerprint
std::array<float, 32> LocalVoiceDetection::extract_audio_fingerprint(const int16_t* samples, size_t count) {
    std::array<float, 32> fingerprint = {0};
    
    if (!samples || count == 0) {
        return fingerprint;
    }
    
    // Convert to float and apply window
    std::vector<float> float_samples(count);
    for (size_t i = 0; i < count; ++i) {
        float_samples[i] = (float)samples[i] / 32768.0f;
    }
    
    apply_window(float_samples.data(), count);
    
    // Compute FFT (simplified - using basic frequency analysis)
    const size_t fft_size = std::min(count, (size_t)FFT_SIZE);
    std::vector<float> fft_result(fft_size);
    
    compute_fft(float_samples.data(), fft_result.data(), fft_size);
    
    // Extract features into fingerprint
    const size_t bins_per_feature = fft_size / 32;
    
    for (size_t i = 0; i < 32 && i * bins_per_feature < fft_size; ++i) {
        float bin_energy = 0.0f;
        size_t start_bin = i * bins_per_feature;
        size_t end_bin = std::min(start_bin + bins_per_feature, fft_size);
        
        for (size_t j = start_bin; j < end_bin; ++j) {
            bin_energy += fft_result[j] * fft_result[j];
        }
        
        fingerprint[i] = std::sqrt(bin_energy / (end_bin - start_bin));
    }
    
    return fingerprint;
}

// Calculate audio energy
float LocalVoiceDetection::calculate_energy(const int16_t* samples, size_t count) {
    if (!samples || count == 0) {
        return 0.0f;
    }
    
    float energy = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float sample = (float)samples[i] / 32768.0f;
        energy += sample * sample;
    }
    
    return energy / count;
}

// Calculate zero crossing rate
float LocalVoiceDetection::calculate_zero_crossing_rate(const int16_t* samples, size_t count) {
    if (!samples || count < 2) {
        return 0.0f;
    }
    
    size_t zero_crossings = 0;
    for (size_t i = 1; i < count; ++i) {
        if ((samples[i] >= 0 && samples[i-1] < 0) || 
            (samples[i] < 0 && samples[i-1] >= 0)) {
            zero_crossings++;
        }
    }
    
    return (float)zero_crossings / count;
}

// Simple command detection using keyword spotting
LocalVoiceDetection::SimpleCommand LocalVoiceDetection::detect_simple_command(const int16_t* samples, size_t count) {
    // This is a very simplified approach - in reality, you'd need proper
    // keyword spotting models or at least better audio feature analysis
    
    // For now, we'll use audio characteristics to guess commands
    float energy = calculate_energy(samples, count);
    float zcr = calculate_zero_crossing_rate(samples, count);
    
    // Simple heuristics based on typical voice characteristics
    if (energy < 0.01f) {
        return SimpleCommand::Unknown;  // Too quiet
    }
    
    // Use spectral characteristics to distinguish commands
    auto fingerprint = extract_audio_fingerprint(samples, count);
    
    // Calculate spectral centroid (frequency center of mass)
    float spectral_centroid = 0.0f;
    float total_energy = 0.0f;
    for (size_t i = 0; i < fingerprint.size(); ++i) {
        spectral_centroid += i * fingerprint[i];
        total_energy += fingerprint[i];
    }
    
    if (total_energy > 0) {
        spectral_centroid /= total_energy;
    }
    
    // Very basic classification based on audio features
    // Note: This is highly simplified and would need proper training in reality
    if (spectral_centroid < 8.0f && energy > 0.05f) {
        return SimpleCommand::Play;
    } else if (spectral_centroid < 12.0f && zcr > 0.1f) {
        return SimpleCommand::Pause;
    } else if (spectral_centroid > 16.0f) {
        return SimpleCommand::Next;
    } else if (energy > 0.08f && zcr < 0.05f) {
        return SimpleCommand::VolumeUp;
    }
    
    return SimpleCommand::Unknown;
}

// Get current energy level
float LocalVoiceDetection::get_current_energy() const {
    if (vad_state_.history_index == 0) {
        return vad_state_.energy_history[9];
    }
    return vad_state_.energy_history[vad_state_.history_index - 1];
}

// Apply window function (Hamming window)
void LocalVoiceDetection::apply_window(float* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float window_value = 0.54f - 0.46f * std::cos(2.0f * M_PI * i / (count - 1));
        samples[i] *= window_value;
    }
}

// Simplified FFT computation (using ESP-DSP when available)
void LocalVoiceDetection::compute_fft(const float* input, float* output, size_t size) {
#ifdef CONFIG_DSP_OPTIMIZED
    // Use ESP-DSP optimized FFT if available
    dsps_fft2r_fc32_ae32(const_cast<float*>(input), output, size);
#else
    // Simple DFT fallback for basic frequency analysis
    for (size_t k = 0; k < size / 2; ++k) {
        float real = 0.0f, imag = 0.0f;
        
        for (size_t n = 0; n < size; ++n) {
            float angle = -2.0f * M_PI * k * n / size;
            real += input[n] * std::cos(angle);
            imag += input[n] * std::sin(angle);
        }
        
        output[k] = std::sqrt(real * real + imag * imag);
    }
#endif
}

// Compare two fingerprints
float LocalVoiceDetection::compare_fingerprints(const std::array<float, 32>& fp1, const std::array<float, 32>& fp2) {
    float dot_product = 0.0f;
    float norm1 = 0.0f, norm2 = 0.0f;
    
    for (size_t i = 0; i < fp1.size(); ++i) {
        dot_product += fp1[i] * fp2[i];
        norm1 += fp1[i] * fp1[i];
        norm2 += fp2[i] * fp2[i];
    }
    
    if (norm1 == 0.0f || norm2 == 0.0f) {
        return 0.0f;
    }
    
    // Cosine similarity
    return dot_product / (std::sqrt(norm1) * std::sqrt(norm2));
}

// Convert command to string
const char* LocalVoiceDetection::simple_command_to_string(SimpleCommand cmd) const {
    switch (cmd) {
        case SimpleCommand::Play: return "Play";
        case SimpleCommand::Pause: return "Pause";
        case SimpleCommand::Stop: return "Stop";
        case SimpleCommand::Next: return "Next";
        case SimpleCommand::Previous: return "Previous";
        case SimpleCommand::VolumeUp: return "VolumeUp";
        case SimpleCommand::VolumeDown: return "VolumeDown";
        default: return "Unknown";
    }
}

// Factory implementations
namespace LocalVoiceDetectionFactory {

std::unique_ptr<LocalVoiceDetection> create_default() {
    auto detector = std::make_unique<LocalVoiceDetection>();
    if (detector->initialize("Hey Speaker")) {
        return detector;
    }
    return nullptr;
}

std::unique_ptr<LocalVoiceDetection> create_with_wake_word(const std::string& wake_word) {
    auto detector = std::make_unique<LocalVoiceDetection>();
    if (detector->initialize(wake_word)) {
        return detector;
    }
    return nullptr;
}

size_t estimate_memory_usage() {
    return LocalVoiceDetection::get_max_memory_usage();
}

} // namespace LocalVoiceDetectionFactory

} // namespace smart_speaker 