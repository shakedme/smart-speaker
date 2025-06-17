#pragma once

#include <memory>
#include <functional>
#include "bluetooth_pipeline.hpp"
#include "spotify_pipeline.hpp"
#include "llm_pipeline.hpp"

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

enum class AudioSource {
    NONE,
    VOICE_RESPONSE,  // Highest priority
    BLUETOOTH,
    SPOTIFY
};

enum class PlaybackState {
    STOPPED,
    PLAYING,
    PAUSED,
    PREPARING,
    ERROR
};

class AudioSourceManager {
public:
    static AudioSourceManager& getInstance();

    // Delete copy operations
    AudioSourceManager(const AudioSourceManager&) = delete;
    AudioSourceManager& operator=(const AudioSourceManager&) = delete;

    // Delete move operations
    AudioSourceManager(AudioSourceManager&&) = delete;
    AudioSourceManager& operator=(AudioSourceManager&&) = delete;

    // Initialization
    esp_err_t initialize();
    esp_err_t start();
    esp_err_t stop();

    void onMusicCommand(const char* command);

    // Source switching
    esp_err_t switchToSource(AudioSource source);

    // Common playback controls (delegates to current active source)
    esp_err_t play();
    esp_err_t pause();
    esp_err_t resume();
    esp_err_t stopPlayback();
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);
    [[nodiscard]] uint8_t getVolume() const { return volume; }

    // Voice response controls (highest priority)
    esp_err_t queueVoiceResponseChunk(const uint8_t* audio_data, size_t length, bool is_final);
    esp_err_t stopVoiceResponse();


    // I2S Hardware lifecycle management

private:
    AudioSourceManager() = default;
    ~AudioSourceManager() = default;

    // Core pipeline components
    std::unique_ptr<BluetoothPipeline> btPipeline;
    std::unique_ptr<SpotifyPipeline> spotifyPipeline;
    std::unique_ptr<LLMPipeline> llmPipeline;

    // State management
    AudioSource currentSource;
    AudioSource previousSource;  // For restoration after voice response
    uint8_t volume;
    bool isInitialized;
    bool autoRestorePreviousSource;

    // I2S Hardware coordination
    enum class I2SHardwareState {
        IDLE,           // No driver installed
        ESP_ADF_ACTIVE, // ESP-ADF has control
        CSPOT_ACTIVE,   // cspot has control
        SWITCHING       // In transition between sources
    };
    
    std::atomic<I2SHardwareState> i2SHardwareState;
    static constexpr int I2S_SWITCH_DELAY_MS = 200;

    // Internal helpers
    esp_err_t stopCurrentSource();
    esp_err_t pauseCurrentSourceForVoice();
    esp_err_t restorePreviousSource();

    void ensureI2SDriverStopped();
    esp_err_t verifyI2SHardwareState();
    esp_err_t startSource(AudioSource source);

    // Spotify-specific controls
    esp_err_t spotifyPlay(const std::string& uri);

};

} // namespace btspeaker 