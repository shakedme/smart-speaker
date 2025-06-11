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

    // Initialize Bluetooth pipeline
    bt_pipeline_ = std::make_unique<AudioPipelineManager>();
    esp_err_t ret = bt_pipeline_->setup_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup Bluetooth pipeline");
        return ret;
    }

    // Initialize Spotify pipeline
    spotify_pipeline_ = std::make_unique<SpotifyPipeline>();
    ret = spotify_pipeline_->initialize();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Spotify pipeline");
        return ret;
    }

    current_source_ = AudioSource::NONE;
    volume_ = 100;
    is_initialized_ = true;

    return ESP_OK;
}

esp_err_t AudioSourceManager::start() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    // Start Bluetooth pipeline
    esp_err_t ret = bt_pipeline_->start_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Bluetooth pipeline");
        return ret;
    }

    // Start Spotify pipeline
    ret = spotify_pipeline_->start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Spotify pipeline");
        return ret;
    }

    return ESP_OK;
}

esp_err_t AudioSourceManager::stop() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    // Stop current source
    esp_err_t ret = stopCurrentSource();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop current source");
        return ret;
    }

    // Stop both pipelines
    bt_pipeline_->stop_pipeline();
    spotify_pipeline_->stop();

    return ESP_OK;
}

esp_err_t AudioSourceManager::switchToSource(AudioSource source) {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (source == current_source_) {
        return ESP_OK;
    }

    // Stop current source
    esp_err_t ret = stopCurrentSource();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop current source");
        return ret;
    }

    // Start new source
    ret = startSource(source);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start new source");
        return ret;
    }

    current_source_ = source;
    return ESP_OK;
}

esp_err_t AudioSourceManager::stopCurrentSource() {
    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->stop_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->stop();
        case AudioSource::NONE:
            return ESP_OK;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::startSource(AudioSource source) {
    switch (source) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->start_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->start();
        case AudioSource::NONE:
            return ESP_OK;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::play() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->resume_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->resume();
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::pause() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->pause_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->pause();
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t AudioSourceManager::resume() {
    return play();
}

esp_err_t AudioSourceManager::stopPlayback() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (current_source_) {
        case AudioSource::BLUETOOTH:
            return bt_pipeline_->stop_pipeline();
        case AudioSource::SPOTIFY:
            return spotify_pipeline_->stop();
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

void AudioSourceManager::setVolume(uint8_t volume) {
    volume_ = volume;
    
    if (bt_pipeline_) {
        // TODO: Implement volume control for Bluetooth
    }
    
    if (spotify_pipeline_) {
        spotify_pipeline_->setVolume(volume);
    }
}

esp_err_t AudioSourceManager::spotifyPlay(const std::string& uri) {
    if (!is_initialized_ || !spotify_pipeline_) {
        return ESP_ERR_INVALID_STATE;
    }

    // Switch to Spotify if not already active
    if (current_source_ != AudioSource::SPOTIFY) {
        esp_err_t ret = switchToSource(AudioSource::SPOTIFY);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return spotify_pipeline_->play(uri);
}

esp_err_t AudioSourceManager::spotifySearch(const std::string& query) {
    if (!is_initialized_ || !spotify_pipeline_) {
        return ESP_ERR_INVALID_STATE;
    }
    return spotify_pipeline_->search(query);
}

} // namespace btspeaker 