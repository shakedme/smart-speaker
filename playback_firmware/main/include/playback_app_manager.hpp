#pragma once

#include "audio_source_manager.hpp"
#include "playback_backend_client.hpp"
#include "playback_reference_tap.hpp"
#include "wifi_manager.hpp"
#include <memory>

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

class PlaybackAppManager {
public:
    PlaybackAppManager();
    ~PlaybackAppManager();

    PlaybackAppManager(const PlaybackAppManager &) = delete;
    PlaybackAppManager &operator=(const PlaybackAppManager &) = delete;

    esp_err_t start();

private:
    static constexpr const char *TAG = "PLAYBACK_APP";

    void initNVS();
    void setupBackendCallbacks();
    void handleBackendIntent(const char *intent, const char *data);

    std::unique_ptr<WiFiManager> wifiManager_;
    std::unique_ptr<AudioSourceManager> audioSourceManager_;
    std::unique_ptr<PlaybackBackendClient> backendClient_;
    std::unique_ptr<PlaybackReferenceTap> playbackRefTap_;
};

}  // namespace btspeaker
