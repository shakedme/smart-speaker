#include "playback_app_manager.hpp"

#include "task_affinity.hpp"
#include "voice_config.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
}

namespace btspeaker {

PlaybackAppManager::PlaybackAppManager() {
    initNVS();
    wifiManager_ = std::make_unique<WiFiManager>();
    wifiManager_->connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    audioSourceManager_ = std::make_unique<AudioSourceManager>(*wifiManager_);
    backendClient_ = std::make_unique<PlaybackBackendClient>();
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    playbackRefTap_ = std::make_unique<PlaybackReferenceTap>();
    playbackRefTap_->setInstance(playbackRefTap_.get());
#endif
    setupBackendCallbacks();
}

PlaybackAppManager::~PlaybackAppManager() {
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    if (playbackRefTap_) {
        playbackRefTap_->setActive(false);
        playbackRefTap_->setInstance(nullptr);
    }
#endif
    stopPlaybackStateReporter();
}

void PlaybackAppManager::initNVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void PlaybackAppManager::setupBackendCallbacks() {
    backendClient_->setAudioDownlinkCallback([this](const uint8_t *data, const size_t length) {
        if (!audioSourceManager_) {
            return;
        }
        const esp_err_t ret = audioSourceManager_->queueVoiceResponseChunk(data, length);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue LLM audio: %s", esp_err_to_name(ret));
        }
    });
    backendClient_->setIntentCallback([this](const char *intent, const char *data) {
        handleBackendIntent(intent, data);
    });
    backendClient_->setStateChangeCallback([this](const char *state) {
        handleBackendStateChange(state);
    });
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    if (playbackRefTap_) {
        playbackRefTap_->setSendCallback([this](const int16_t *pcm,
                                                const uint32_t num_samples,
                                                const uint32_t sequence,
                                                const int64_t timestamp_us) -> bool {
            if (!backendClient_ || !backendClient_->isConnected()) {
                return false;
            }
            const esp_err_t ret = backendClient_->sendPlaybackRef(
                    pcm,
                    num_samples,
                    kPlaybackRefOutputSampleRate,
                    1,
                    sequence,
                    timestamp_us);
            return ret == ESP_OK;
        });
    }
#endif
}

void PlaybackAppManager::handleBackendIntent(const char *intent, const char *data) {
    if (!audioSourceManager_ || !intent || !data) {
        return;
    }
    if (std::strcmp(intent, "spotify") == 0 || std::strcmp(intent, "playback_control") == 0) {
        audioSourceManager_->onMusicCommand(data);
    }
}

void PlaybackAppManager::handleBackendStateChange(const char *state) {
    if (!audioSourceManager_ || !state) {
        return;
    }
    if (std::strcmp(state, "STREAMING_RESPONSE") != 0) {
        restoreVoiceResponseWhenIdle();
    }
}

void PlaybackAppManager::restoreVoiceResponseWhenIdle() {
    if (!audioSourceManager_ ||
        audioSourceManager_->getCurrentSource() != AudioSource::VOICE_RESPONSE) {
        return;
    }

    constexpr int kMaxWaitMs = 5000;
    constexpr int kPollMs = 50;

    int waited_ms = 0;
    while (audioSourceManager_->getCurrentSource() == AudioSource::VOICE_RESPONSE &&
           !audioSourceManager_->isVoiceResponsePlaybackIdle() &&
           waited_ms < kMaxWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        waited_ms += kPollMs;
    }

    if (audioSourceManager_->getCurrentSource() == AudioSource::VOICE_RESPONSE) {
        const esp_err_t ret = audioSourceManager_->stopVoiceResponse();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore source after voice response: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t PlaybackAppManager::start() {
    esp_err_t ret = audioSourceManager_->start();
    if (ret != ESP_OK) {
        return ret;
    }
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    if (playbackRefTap_) {
        playbackRefTap_->setActive(true);
    }
#endif
    startPlaybackStateReporter();
    if (wifiManager_->isConnected()) {
        ret = backendClient_->start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Backend client start failed: %s", esp_err_to_name(ret));
        }
    }
    return ESP_OK;
}

void PlaybackAppManager::startPlaybackStateReporter() {
    if (playbackStateTaskHandle_ != nullptr) {
        return;
    }
    if (xTaskCreatePinnedToCore(playbackStateReporterTask, "pb_state_tx", 4096, this,
                                kWsReconnectPriority, &playbackStateTaskHandle_, kIoCore) != pdPASS) {
        playbackStateTaskHandle_ = nullptr;
        ESP_LOGE(TAG, "Failed to start playback state reporter");
    }
}

void PlaybackAppManager::stopPlaybackStateReporter() {
    if (playbackStateTaskHandle_ != nullptr) {
        vTaskDelete(playbackStateTaskHandle_);
        playbackStateTaskHandle_ = nullptr;
    }
}

void PlaybackAppManager::publishPlaybackStateIfChanged() {
    if (!backendClient_ || !backendClient_->isConnected() || !audioSourceManager_) {
        return;
    }

    AudioSourceManager::PlaybackUiSnapshot snapshot{};
    if (!audioSourceManager_->fillPlaybackUiSnapshot(snapshot)) {
        return;
    }

    char key[sizeof(lastPublishedKey_)];
    std::snprintf(key, sizeof(key), "%s|%s|%s|%s|%d",
                  snapshot.source,
                  snapshot.playback_state,
                  snapshot.title,
                  snapshot.artist,
                  snapshot.duration_ms);
    if (std::strcmp(key, lastPublishedKey_) == 0) {
        return;
    }
    std::strncpy(lastPublishedKey_, key, sizeof(lastPublishedKey_) - 1);
    lastPublishedKey_[sizeof(lastPublishedKey_) - 1] = '\0';

    const esp_err_t ret = backendClient_->publishPlaybackState(
            snapshot.source,
            snapshot.playback_state,
            snapshot.title[0] != '\0' ? snapshot.title : nullptr,
            snapshot.artist[0] != '\0' ? snapshot.artist : nullptr,
            snapshot.album[0] != '\0' ? snapshot.album : nullptr,
            snapshot.duration_ms,
            snapshot.position_ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish playback state: %s", esp_err_to_name(ret));
    }
}

void PlaybackAppManager::playbackStateReporterTask(void *arg) {
    auto *manager = static_cast<PlaybackAppManager *>(arg);
    while (true) {
        manager->publishPlaybackStateIfChanged();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace btspeaker
