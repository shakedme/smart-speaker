#include "voice_ui_app_manager.hpp"

#include "korvo2_lcd_display_backend.hpp"
#include "korvo2_mic_capture_backend.hpp"
#include "production_spi_lcd_display_backend.hpp"
#include "sph0645_dual_mic_capture_backend.hpp"
#include "voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
}

namespace btspeaker {

VoiceUiAppManager::VoiceUiAppManager() {
    initNVS();
    wifiManager_ = std::make_unique<WiFiManager>();
    micBackend_ = createMicBackend();
    displayBackend_ = createDisplayBackend();
    uiApp_ = std::make_unique<UiApp>(*displayBackend_);
    backendClient_ = std::make_unique<VoiceBackendClient>();
    wireCallbacks();
    wifiManager_->setStatusCallback([this](const WiFiManager::Status status) {
        onWifiStatus(status);
    });
    wifiManager_->connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
}

VoiceUiAppManager::~VoiceUiAppManager() {
    if (micBackend_) {
        micBackend_->stop();
    }
    if (backendClient_) {
        backendClient_->stop();
    }
}

void VoiceUiAppManager::initNVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

std::unique_ptr<MicCaptureBackend> VoiceUiAppManager::createMicBackend() {
#if defined(VOICE_UI_BOARD_PROFILE_S3_N32R16_SPH0645_LCD)
    return std::make_unique<Sph0645DualMicCaptureBackend>();
#else
    return std::make_unique<Korvo2MicCaptureBackend>();
#endif
}

std::unique_ptr<DisplayBackend> VoiceUiAppManager::createDisplayBackend() {
#if defined(VOICE_UI_BOARD_PROFILE_S3_N32R16_SPH0645_LCD)
    return std::make_unique<ProductionSpiLcdDisplayBackend>();
#else
    return std::make_unique<Korvo2LcdDisplayBackend>();
#endif
}

void VoiceUiAppManager::onWifiStatus(const WiFiManager::Status status) {
    switch (status) {
        case WiFiManager::Status::CONNECTING:
            uiApp_->setConnecting();
            break;
        case WiFiManager::Status::CONNECTED:
            ESP_LOGI(TAG, "WiFi connected, starting backend client");
            if (backendClient_) {
                const esp_err_t ret = backendClient_->start();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Backend client start failed: %s", esp_err_to_name(ret));
                    uiApp_->setConnecting();
                }
            }
            break;
        case WiFiManager::Status::DISCONNECTED:
        case WiFiManager::Status::FAILED:
            if (backendClient_) {
                backendClient_->stop();
            }
            uiApp_->setConnecting();
            break;
        default:
            break;
    }
}

void VoiceUiAppManager::wireCallbacks() {
    backendClient_->setNowPlayingCallback([this](const NowPlayingUiState &state) {
        uiApp_->updateNowPlaying(state);
    });
    backendClient_->setWaveformCallback([this](const WaveformUiState &state) {
        uiApp_->updateWaveform(state);
    });
    backendClient_->setSessionStateCallback([this](const char *state) {
        ESP_LOGI(TAG, "session_state=%s", state ? state : "unknown");
        uiApp_->updateSessionState(state);
    });
}

esp_err_t VoiceUiAppManager::start() {
    esp_err_t ret = uiApp_->init();
    if (ret != ESP_OK) {
        return ret;
    }
    uiApp_->setConnecting();

    ret = micBackend_->init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = micBackend_->start([this](const int16_t *samples, const size_t sample_count, const int64_t timestamp_us) {
        if (backendClient_ && backendClient_->isConnected()) {
            (void) backendClient_->sendMicFrame(samples,
                                                static_cast<uint32_t>(sample_count),
                                                micSequence_++,
                                                timestamp_us);
        }
    });
    if (ret != ESP_OK) {
        return ret;
    }

    if (wifiManager_->isConnected()) {
        ret = backendClient_->start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Backend client start failed: %s", esp_err_to_name(ret));
        }
    }
    return ESP_OK;
}

}  // namespace btspeaker
