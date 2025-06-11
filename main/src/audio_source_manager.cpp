#include "../include/audio_source_manager.hpp"
#include "esp_log.h"

namespace btspeaker {

static constexpr const char* TAG = "AUDIO_SOURCE_MGR";

AudioSourceManager& AudioSourceManager::getInstance() {
    static AudioSourceManager instance;
    return instance;
}

esp_err_t AudioSourceManager::initialize() {
    if (is_initialized_) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing AudioSourceManager");

    // Initialize Bluetooth pipeline
    bt_pipeline_ = std::make_unique<AudioPipelineManager>();
    esp_err_t ret = bt_pipeline_->setup_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup Bluetooth pipeline: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize Spotify pipeline
    spotify_pipeline_ = std::make_unique<SpotifyPipeline>();
    ret = spotify_pipeline_->initialize();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Spotify pipeline: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize voice response player
    voice_response_player_ = std::make_unique<AudioResponsePlayer>();
    ret = voice_response_player_->init(bt_pipeline_.get());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize voice response player: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize state
    current_source_ = AudioSource::NONE;
    previous_source_ = AudioSource::NONE;
    current_state_ = PlaybackState::STOPPED;
    volume_ = 100;
    auto_restore_previous_source_ = true;
    is_initialized_ = true;

    ESP_LOGI(TAG, "AudioSourceManager initialized successfully");
    return ESP_OK;
}

esp_err_t AudioSourceManager::start() {
    if (!is_initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting AudioSourceManager");

    // Start Bluetooth pipeline (always available)
    esp_err_t ret = bt_pipeline_->start_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Bluetooth pipeline: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start Spotify pipeline (will start when needed)
    ret = spotify_pipeline_->start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Spotify pipeline: %s", esp_err_to_name(ret));
        // Continue - Spotify may not be available
    }

    // Default to Bluetooth source
    ret = switchToSource(AudioSource::BLUETOOTH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to Bluetooth source: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "AudioSourceManager started successfully");
    return ESP_OK;
}

esp_err_t AudioSourceManager::stop() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping AudioSourceManager");

    // Stop current source
    esp_err_t ret = stopCurrentSource();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop current source: %s", esp_err_to_name(ret));
    }

    // Stop all pipelines
    if (bt_pipeline_) {
        bt_pipeline_->stop_pipeline();
    }
    if (spotify_pipeline_) {
        spotify_pipeline_->stop();
    }
    if (voice_response_player_) {
        voice_response_player_->stop_playback();
    }

    current_source_ = AudioSource::NONE;
    current_state_ = PlaybackState::STOPPED;

    ESP_LOGI(TAG, "AudioSourceManager stopped");
    return ESP_OK;
}

esp_err_t AudioSourceManager::switchToSource(AudioSource source) {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (source == current_source_) {
        ESP_LOGD(TAG, "Already using source %d", (int)source);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching from source %d to %d", (int)current_source_, (int)source);

    AudioSource old_source = current_source_;

    // Stop current source (unless it's voice response - that has priority)
    if (current_source_ != AudioSource::VOICE_RESPONSE) {
        esp_err_t ret = stopCurrentSource();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop current source: %s", esp_err_to_name(ret));
        }
    }

    // Store previous source for restoration after voice response
    if (source == AudioSource::VOICE_RESPONSE && current_source_ != AudioSource::VOICE_RESPONSE) {
        previous_source_ = current_source_;
        ESP_LOGD(TAG, "Storing previous source %d for restoration", (int)previous_source_);
    }

    // Start new source
    esp_err_t ret = startSource(source);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start new source %d: %s", (int)source, esp_err_to_name(ret));
        return ret;
    }

    current_source_ = source;
    updatePlaybackState();
    notifySourceChange(old_source, source);

    ESP_LOGI(TAG, "Successfully switched to source %d", (int)source);
    return ESP_OK;
}

esp_err_t AudioSourceManager::stopCurrentSource() {
    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->pause_pipeline();  // Pause instead of stop to maintain connection
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->pause();      // Pause to maintain Spotify session
        case AudioSource::VOICE_RESPONSE:
            return voice_response_player_->stop_playback();
        case AudioSource::NONE:
            return ESP_OK;
        default:
            ESP_LOGW(TAG, "Unknown source %d", (int)current_source_);
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::startSource(AudioSource source) {
    switch (source) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->resume_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->resume();
        case AudioSource::VOICE_RESPONSE:
            return ESP_OK;  // Voice response player is started when audio is queued
        case AudioSource::NONE:
            return ESP_OK;
        default:
            ESP_LOGW(TAG, "Unknown source %d", (int)source);
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::pauseCurrentSourceForVoice() {
    if (current_source_ == AudioSource::VOICE_RESPONSE) {
        return ESP_OK;  // Already in voice mode
    }

    previous_source_ = current_source_;
    ESP_LOGD(TAG, "Pausing source %d for voice response", (int)current_source_);
    
    return stopCurrentSource();
}

esp_err_t AudioSourceManager::restorePreviousSource() {
    if (previous_source_ == AudioSource::NONE || !auto_restore_previous_source_) {
        ESP_LOGD(TAG, "No previous source to restore or auto-restore disabled");
        current_source_ = AudioSource::NONE;
        current_state_ = PlaybackState::STOPPED;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restoring previous source %d", (int)previous_source_);
    
    AudioSource restore_source = previous_source_;
    previous_source_ = AudioSource::NONE;
    
    return switchToSource(restore_source);
}

// ===== PLAYBACK CONTROL METHODS =====

esp_err_t AudioSourceManager::play() {
    if (!is_initialized_ || current_source_ == AudioSource::NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            ret = bt_pipeline_->resume_pipeline();
            break;
        case AudioSource::SPOTIFY:
            ret = spotify_pipeline_->resume();
            break;
        case AudioSource::VOICE_RESPONSE:
            ret = voice_response_player_->resume_playback();
            break;
        default:
            ret = ESP_ERR_INVALID_STATE;
    }

    if (ret == ESP_OK) {
        current_state_ = PlaybackState::PLAYING;
    }
    return ret;
}

esp_err_t AudioSourceManager::pause() {
    if (!is_initialized_ || current_source_ == AudioSource::NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            ret = bt_pipeline_->pause_pipeline();
            break;
        case AudioSource::SPOTIFY:
            ret = spotify_pipeline_->pause();
            break;
        case AudioSource::VOICE_RESPONSE:
            ret = voice_response_player_->pause_playback();
            break;
        default:
            ret = ESP_ERR_INVALID_STATE;
    }

    if (ret == ESP_OK) {
        current_state_ = PlaybackState::PAUSED;
    }
    return ret;
}

esp_err_t AudioSourceManager::resume() {
    return play();
}

esp_err_t AudioSourceManager::stopPlayback() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = stopCurrentSource();
    if (ret == ESP_OK) {
        current_state_ = PlaybackState::STOPPED;
    }
    return ret;
}

esp_err_t AudioSourceManager::skipNext() {
    if (!is_initialized_ || current_source_ == AudioSource::NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (current_source_) {
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->skipNext();
        case AudioSource::BLUETOOTH:
            // Bluetooth doesn't support skip (would need AVRCP implementation)
            ESP_LOGW(TAG, "Skip not supported for Bluetooth source");
            return ESP_ERR_NOT_SUPPORTED;
        case AudioSource::VOICE_RESPONSE:
            ESP_LOGW(TAG, "Skip not supported for voice responses");
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::skipPrevious() {
    if (!is_initialized_ || current_source_ == AudioSource::NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (current_source_) {
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->skipPrevious();
        case AudioSource::BLUETOOTH:
            ESP_LOGW(TAG, "Skip not supported for Bluetooth source");
            return ESP_ERR_NOT_SUPPORTED;
        case AudioSource::VOICE_RESPONSE:
            ESP_LOGW(TAG, "Skip not supported for voice responses");
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

void AudioSourceManager::setVolume(uint8_t volume) {
    volume_ = volume;
    
    ESP_LOGI(TAG, "Setting volume to %d", volume);
    
    // Apply volume to all sources
    if (bt_pipeline_) {
        // TODO: Implement Bluetooth volume control
        ESP_LOGD(TAG, "Bluetooth volume control not implemented");
    }
    
    if (spotify_pipeline_) {
        spotify_pipeline_->setVolume(volume);
    }
    
    if (voice_response_player_) {
        // TODO: Implement voice response volume control if needed
        ESP_LOGD(TAG, "Voice response volume control not implemented");
    }
}

// ===== VOICE RESPONSE METHODS =====

esp_err_t AudioSourceManager::playVoiceResponse(const uint8_t* audio_data, size_t length) {
    if (!is_initialized_ || !voice_response_player_) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing voice response (length: %zu)", length);

    // Pause current source for voice response
    esp_err_t ret = pauseCurrentSourceForVoice();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to pause current source: %s", esp_err_to_name(ret));
    }

    // Switch to voice response source
    ret = switchToSource(AudioSource::VOICE_RESPONSE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to voice response: %s", esp_err_to_name(ret));
        return ret;
    }

    // Play the audio response
    ret = voice_response_player_->play_audio_response(audio_data, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play voice response: %s", esp_err_to_name(ret));
        // Try to restore previous source
        restorePreviousSource();
        return ret;
    }

    current_state_ = PlaybackState::PLAYING;
    return ESP_OK;
}

esp_err_t AudioSourceManager::queueVoiceResponseChunk(const uint8_t* audio_data, size_t length, bool is_final) {
    if (!is_initialized_ || !voice_response_player_) {
        return ESP_ERR_INVALID_STATE;
    }

    // If this is the first chunk, set up voice response mode
    if (current_source_ != AudioSource::VOICE_RESPONSE) {
        esp_err_t ret = pauseCurrentSourceForVoice();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to pause current source: %s", esp_err_to_name(ret));
        }

        ret = switchToSource(AudioSource::VOICE_RESPONSE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to voice response: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_err_t ret = voice_response_player_->queue_audio_chunk(audio_data, length, is_final);
    if (ret == ESP_OK) {
        current_state_ = PlaybackState::PLAYING;
        
        // If this is the final chunk, set up restoration
        if (is_final) {
            ESP_LOGD(TAG, "Final voice response chunk queued");
            // Note: Restoration will happen automatically when voice response completes
        }
    }

    return ret;
}

esp_err_t AudioSourceManager::stopVoiceResponse() {
    if (current_source_ != AudioSource::VOICE_RESPONSE) {
        return ESP_OK;  // Not playing voice response
    }

    ESP_LOGI(TAG, "Stopping voice response");

    esp_err_t ret = voice_response_player_->stop_playback();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop voice response: %s", esp_err_to_name(ret));
    }

    // Restore previous source
    ret = restorePreviousSource();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore previous source: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

// ===== SPOTIFY METHODS =====

esp_err_t AudioSourceManager::spotifyPlay(const std::string& uri) {
    if (!is_initialized_ || !spotify_pipeline_) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing Spotify URI: %s", uri.c_str());

    // Switch to Spotify if not already active
    if (current_source_ != AudioSource::SPOTIFY) {
        esp_err_t ret = switchToSource(AudioSource::SPOTIFY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to Spotify: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_err_t ret = spotify_pipeline_->play(uri);
    if (ret == ESP_OK) {
        current_state_ = PlaybackState::PLAYING;
    }
    return ret;
}

esp_err_t AudioSourceManager::spotifySearch(const std::string& query) {
    if (!is_initialized_ || !spotify_pipeline_) {
        return ESP_ERR_INVALID_STATE;
    }
    return spotify_pipeline_->search(query);
}

// ===== STATE QUERY METHODS =====

PlaybackState AudioSourceManager::getPlaybackState() const {
    return current_state_;
}

bool AudioSourceManager::isPlaying() const {
    return current_state_ == PlaybackState::PLAYING;
}

bool AudioSourceManager::isVoiceResponsePlaying() const {
    return current_source_ == AudioSource::VOICE_RESPONSE && 
           voice_response_player_ && 
           voice_response_player_->is_playing();
}

// ===== CALLBACK MANAGEMENT =====

void AudioSourceManager::setSourceChangeCallback(SourceChangeCallback callback) {
    source_change_callback_ = callback;
}

// ===== INTERNAL HELPERS =====

void AudioSourceManager::updatePlaybackState() {
    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            // TODO: Get actual state from Bluetooth pipeline
            current_state_ = PlaybackState::PLAYING;
            break;
        case AudioSource::SPOTIFY:
            current_state_ = spotify_pipeline_->isPlaying() ? PlaybackState::PLAYING : PlaybackState::PAUSED;
            break;
        case AudioSource::VOICE_RESPONSE:
            if (voice_response_player_) {
                switch (voice_response_player_->get_state()) {
                    case AudioResponsePlayer::State::PLAYING:
                        current_state_ = PlaybackState::PLAYING;
                        break;
                    case AudioResponsePlayer::State::PAUSED:
                        current_state_ = PlaybackState::PAUSED;
                        break;
                    case AudioResponsePlayer::State::PREPARING:
                        current_state_ = PlaybackState::PREPARING;
                        break;
                    case AudioResponsePlayer::State::ERROR:
                        current_state_ = PlaybackState::ERROR;
                        break;
                    default:
                        current_state_ = PlaybackState::STOPPED;
                }
            }
            break;
        case AudioSource::NONE:
            current_state_ = PlaybackState::STOPPED;
            break;
    }
}

void AudioSourceManager::notifySourceChange(AudioSource from, AudioSource to) {
    if (source_change_callback_) {
        source_change_callback_(from, to);
    }
}

void AudioSourceManager::onVoiceResponseComplete(void* user_data) {
    if (user_data) {
        static_cast<AudioSourceManager*>(user_data)->handleVoiceResponseComplete();
    }
}

void AudioSourceManager::handleVoiceResponseComplete() {
    ESP_LOGI(TAG, "Voice response completed, restoring previous source");
    
    esp_err_t ret = restorePreviousSource();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore previous source: %s", esp_err_to_name(ret));
    }
}

} // namespace btspeaker 