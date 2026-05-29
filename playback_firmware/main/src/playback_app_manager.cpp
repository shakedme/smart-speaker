#include "playback_app_manager.hpp"

#include "voice_config.hpp"
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
    if (wifiManager_->isConnected()) {
        ret = backendClient_->start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Backend client start failed: %s", esp_err_to_name(ret));
        }
    }
    return ESP_OK;
}

}  // namespace btspeaker
