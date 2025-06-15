#pragma once

#include "esp_raii_wrapper.hpp"
#include <memory>
#include <mutex>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "board.h"
}

namespace btspeaker {

/**
 * @brief Shared I2S Audio Service for centralized audio output management
 * 
 * This service provides a single point of I2S configuration and management for all
 * audio pipelines (Bluetooth, Spotify, LLM). It follows the DRY principle by 
 * eliminating duplicate I2S setup code and ensures consistent hardware configuration.
 * 
 * Key Features:
 * - Singleton pattern for system-wide I2S management
 * - Thread-safe audio source switching
 * - Consistent GPIO pin configuration (board.h)
 * - Automatic I2S driver lifecycle management
 * - Support for multiple audio sources without conflicts
 */
class I2SAudioService {
public:
    enum class AudioSource {
        NONE,
        BLUETOOTH,
        SPOTIFY, 
        VOICE_RESPONSE
    };

    // Singleton access
    static I2SAudioService& getInstance();
    
    // Core lifecycle
    esp_err_t initialize();
    esp_err_t shutdown();

    // Audio stream creation for pipelines
    audio_element_handle_t createI2SWriter(AudioSource source, 
                                          int sample_rate = 44100,
                                          int channels = 2,
                                          int bits_per_sample = 16);
    
    // Source management
    esp_err_t setActiveSource(AudioSource source);

private:
    I2SAudioService() = default;
    ~I2SAudioService() = default;
    
    // Singleton management
    I2SAudioService(const I2SAudioService&) = delete;
    I2SAudioService& operator=(const I2SAudioService&) = delete;
    
    // Internal I2S management

    esp_err_t deinitializeI2SDriver();
    i2s_stream_cfg_t createI2SConfig(AudioSource source, int sampleRate, int channels, int bitsPerSample);
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // State management
    bool isInitialized_;
    AudioSource activeSource_;
    uint8_t masterVolume_;
    
    // I2S driver state
    bool i2SDriverInstalled_;
    int currentSampleRate_;
    int currentChannels_;
    int currentBitsPerSample_;
    
    // Constants
    static constexpr const char* TAG = "I2S_AUDIO_SERVICE";
    static constexpr int I2S_PORT = I2S_NUM_0;
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;
    static constexpr int DEFAULT_CHANNELS = 2;
    static constexpr int DEFAULT_BITS = 16;
    static constexpr uint8_t DEFAULT_VOLUME = 100;
};

} // namespace btspeaker 