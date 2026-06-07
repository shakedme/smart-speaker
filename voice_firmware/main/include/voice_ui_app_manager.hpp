#pragma once

#include "display_backend.hpp"
#include "mic_capture_backend.hpp"
#include "ui_app.hpp"
#include "voice_backend_client.hpp"
#include "wifi_manager.hpp"
#include <memory>

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

class VoiceUiAppManager {
public:
    VoiceUiAppManager();
    ~VoiceUiAppManager();

    esp_err_t start();

private:
    static constexpr const char *TAG = "VOICE_UI_APP";

    void initNVS();
    void wireCallbacks();
    void onWifiStatus(WiFiManager::Status status);
    std::unique_ptr<MicCaptureBackend> createMicBackend();
    std::unique_ptr<DisplayBackend> createDisplayBackend();

    // wifiManager_ is declared last so it is destroyed first during teardown.
    // Its disconnect path invokes onWifiStatus, which must run while uiApp_ is alive.
    std::unique_ptr<VoiceBackendClient> backendClient_;
    std::unique_ptr<UiApp> uiApp_;
    std::unique_ptr<DisplayBackend> displayBackend_;
    std::unique_ptr<MicCaptureBackend> micBackend_;
    std::unique_ptr<WiFiManager> wifiManager_;
    uint32_t micSequence_ = 0;
    bool appStarted_ = false;
};

}  // namespace btspeaker
