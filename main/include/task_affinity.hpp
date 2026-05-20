#pragma once

#include "sdkconfig.h"

namespace btspeaker {

// Core 1: dedicated periodic I2S mic capture (mic_audio only).
constexpr int kCaptureCore = 1;

// Core 0: WiFi/BT (sdkconfig), ADF playback I2S, Spotify/cspot.
constexpr int kIoCore = 0;

// Highest app priority on capture core; cspot decode stays below this.
constexpr int kMicAudioPriority = 10;
constexpr int kMicUplinkPriority = 5;
constexpr int kVoiceInboundPriority = 12;
constexpr int kWsReconnectPriority = 5;

// Runs below mic_audio on the capture core so Spotify decode cannot starve
// uplink encoding/dispatch, while mic I2S capture still wins scheduling.
constexpr int kMicUplinkCore = kCaptureCore;

constexpr int kSpotifyMainPriority =
        CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + 6;
constexpr int kSpotifyMainCore = kIoCore;

// Keep cspot decode/preload off the capture core while profiling concurrent mic RX.
constexpr int kCspotPlayerCore = kIoCore;
constexpr int kCspotTrackQueueCore = kIoCore;
constexpr int kCspotMercuryCore = kIoCore;

// Logs expected task layout at boot for field debugging.
void logTaskAffinityLayout();

}  // namespace btspeaker
