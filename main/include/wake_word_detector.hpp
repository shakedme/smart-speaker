#pragma once

#include <memory>
#include <functional>

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

// Callback type for wake word detection
using WakeWordCallback = std::function<void()>;

/**
 * @brief Manages wake word detection using ESP-SR
 * 
 * Responsible for:
 * - ESP-SR initialization and configuration
 * - Wake word model loading
 * - Audio processing for wake word detection
 * - Callback notification when wake word is detected
 */
class WakeWordDetector {
public:
    WakeWordDetector();
    ~WakeWordDetector();
    
    // Delete copy operations
    WakeWordDetector(const WakeWordDetector&) = delete;
    WakeWordDetector& operator=(const WakeWordDetector&) = delete;
    
    /**
     * @brief Initialize wake word detection
     */
    esp_err_t init();
    
    /**
     * @brief Shutdown wake word detector
     */
    void shutdown();
    
    /**
     * @brief Process audio data for wake word detection
     * @param audio_data Raw audio samples (16-bit PCM)
     * @param sample_count Number of samples in the buffer
     * @return true if wake word detected, false otherwise
     */
    bool process_audio(const int16_t* audio_data, size_t sample_count);
    
    /**
     * @brief Set callback for wake word detection
     */
    void set_wake_word_callback(WakeWordCallback callback);
    
    /**
     * @brief Enable/disable wake word detection
     */
    void set_enabled(bool enabled) { enabled_ = enabled; }
    
    /**
     * @brief Check if wake word detection is enabled
     */
    bool is_enabled() const { return enabled_; }
    
    /**
     * @brief Set wake word detection threshold
     */
    void set_threshold(float threshold) { threshold_ = threshold; }

private:
    static constexpr const char* TAG = "WAKE_WORD";
    
    // ESP-SR related setup (currently stubbed)
    esp_err_t setup_esp_sr();
    void cleanup_esp_sr();
    
    // Member variables
    bool enabled_;
    float threshold_;
    WakeWordCallback wake_word_callback_;
    
    // ESP-SR handles (currently nullptr - to be implemented when ESP-SR is ready)
    void* afe_handle_;
    void* afe_data_;
    void* wake_word_models_;
};

} // namespace btspeaker 