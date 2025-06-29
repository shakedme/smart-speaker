#pragma once

#include <memory>
#include <functional>
#include "bluetooth_pipeline.hpp"
#include "spotify_pipeline.hpp"
#include "llm_pipeline.hpp"
#include "wifi_manager.hpp"
#include "audio_types.hpp"

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {


class AudioSourceManager {
public:
    explicit AudioSourceManager(WiFiManager& wifiManager);
    ~AudioSourceManager() = default;

    esp_err_t start();

    void onMusicCommand(const char* command);

    esp_err_t switchToSource(AudioSource source);

    // Common playback controls (delegates to current active source)
    esp_err_t play();
    esp_err_t pause();
    esp_err_t stopPlayback();
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);
    [[nodiscard]] uint8_t getVolume() const { return volume; }

    esp_err_t queueVoiceResponseChunk(const uint8_t* audio_data, size_t length);
    esp_err_t stopVoiceResponse();

private:
    // Core pipeline components
    std::unique_ptr<BluetoothPipeline> btPipeline;
    std::unique_ptr<SpotifyPipeline> spotifyPipeline;
    std::unique_ptr<LLMPipeline> llmPipeline;

    WiFiManager& wifiManager;  // Reference to WiFi manager for network operations

    // State management
    AudioSource currentSource;
    AudioSource previousSource;  // For restoration after voice response
    uint8_t volume;
    bool isInitialized;

    static constexpr int I2S_SWITCH_DELAY_MS = 200;

    // Internal helpers
    esp_err_t stopCurrentSource();
    void updatePreviousSource();
    esp_err_t restorePreviousSource();

    void ensureI2SDriverStopped();
    void startSource(AudioSource source);

    // Spotify-specific controls
    esp_err_t spotifyPlay(const std::string& uri);

};

} // namespace btspeaker 