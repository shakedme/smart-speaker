#pragma once

#include <memory>
#include "audio_pipeline_manager.hpp"
#include "spotify_pipeline.hpp"

namespace btspeaker {

enum class AudioSource {
    BLUETOOTH,
    SPOTIFY,
    NONE
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

    // Source switching
    esp_err_t switchToSource(AudioSource source);
    AudioSource getCurrentSource() const { return current_source_; }

    // Common playback controls
    esp_err_t play();
    esp_err_t pause();
    esp_err_t resume();
    esp_err_t stopPlayback();
    void setVolume(uint8_t volume);

    // Spotify-specific controls
    esp_err_t spotifyPlay(const std::string& uri);
    esp_err_t spotifySearch(const std::string& query);

private:
    AudioSourceManager() = default;
    ~AudioSourceManager() = default;

    std::unique_ptr<AudioPipelineManager> bt_pipeline_;
    std::unique_ptr<SpotifyPipeline> spotify_pipeline_;
    AudioSource current_source_;
    uint8_t volume_;
    bool is_initialized_;

    // Internal helpers
    esp_err_t stopCurrentSource();
    esp_err_t startSource(AudioSource source);
};

} // namespace btspeaker 