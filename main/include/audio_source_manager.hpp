#pragma once

#include <memory>
#include <functional>
#include "audio_pipeline_manager.hpp"
#include "spotify_pipeline.hpp"
#include "audio_response_player.hpp"

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

    // Source switching
    esp_err_t switchToSource(AudioSource source);
    AudioSource getCurrentSource() const { return current_source_; }
    AudioSource getPreviousSource() const { return previous_source_; }

    // Common playback controls (delegates to current active source)
    esp_err_t play();
    esp_err_t pause();
    esp_err_t resume();
    esp_err_t stopPlayback();
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);
    uint8_t getVolume() const { return volume_; }

    // Voice response controls (highest priority)
    esp_err_t playVoiceResponse(const uint8_t* audio_data, size_t length);
    esp_err_t queueVoiceResponseChunk(const uint8_t* audio_data, size_t length, bool is_final);
    esp_err_t stopVoiceResponse();

    // Spotify-specific controls
    esp_err_t spotifyPlay(const std::string& uri);
    esp_err_t spotifySearch(const std::string& query);

    // State query
    PlaybackState getPlaybackState() const;
    bool isPlaying() const;
    bool isVoiceResponsePlaying() const;

    // Callback registration for state changes
    using SourceChangeCallback = std::function<void(AudioSource from, AudioSource to)>;
    void setSourceChangeCallback(SourceChangeCallback callback);

private:
    AudioSourceManager() = default;
    ~AudioSourceManager() = default;

    // Core pipeline components
    std::unique_ptr<AudioPipelineManager> bt_pipeline_;
    std::unique_ptr<SpotifyPipeline> spotify_pipeline_;
    std::unique_ptr<AudioResponsePlayer> voice_response_player_;

    // State management
    AudioSource current_source_;
    AudioSource previous_source_;  // For restoration after voice response
    PlaybackState current_state_;
    uint8_t volume_;
    bool is_initialized_;
    bool auto_restore_previous_source_;

    // Callback for state changes
    SourceChangeCallback source_change_callback_;

    // Internal helpers
    esp_err_t stopCurrentSource();
    esp_err_t startSource(AudioSource source);
    esp_err_t pauseCurrentSourceForVoice();
    esp_err_t restorePreviousSource();
    
    // Voice response completion callback
    static void onVoiceResponseComplete(void* user_data);
    void handleVoiceResponseComplete();
    
    // State synchronization
    void updatePlaybackState();
    void notifySourceChange(AudioSource from, AudioSource to);
};

} // namespace btspeaker 