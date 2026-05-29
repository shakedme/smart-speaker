#include "../include/task_affinity.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

namespace {
constexpr const char *TAG = "TASK_AFFINITY";
}

void logTaskAffinityLayout() {
    ESP_LOGI(TAG,
             "Task layout: capture core=%d (mic_audio prio=%u, mic_uplink core=%d prio=%u); "
             "IO core=%d (voice_inbound=%u spotify_main=%d ws_reconnect=%u)",
             kCaptureCore,
             static_cast<unsigned>(kMicAudioPriority),
             kMicUplinkCore,
             static_cast<unsigned>(kMicUplinkPriority),
             kIoCore,
             static_cast<unsigned>(kVoiceInboundPriority),
             kSpotifyMainPriority,
             static_cast<unsigned>(kWsReconnectPriority));
    ESP_LOGI(TAG,
             "cspot hybrid: player+TrackQueue core %d (below mic prio %u), Mercury core %d; "
             "ADF I2S writer prio 25-27 on core 0",
             kCspotPlayerCore,
             static_cast<unsigned>(kMicAudioPriority),
             kCspotMercuryCore);
}

}  // namespace btspeaker
