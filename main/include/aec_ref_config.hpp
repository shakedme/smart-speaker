#pragma once

#include <cstdint>

// Backend AEC playback-reference uplink. Set to 1 to enable framed mic + playback_ref
// WebSocket streams; 0 preserves legacy uplink behavior.
#ifndef CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
#define CONFIG_BTSPK_BACKEND_AEC_REF_STREAM 0
#endif

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM

namespace btspeaker {

inline constexpr uint32_t kPlaybackRefOutputSampleRate = 16000;
// 320 ms @ 16 kHz. The backend splits to 10 ms before WebRTC AEC3; larger packets
// lower ESP32 WebSocket write pressure while keeping the render stream continuous.
inline constexpr uint32_t kPlaybackRefFrameSamples = 5120;
inline constexpr uint32_t kPlaybackRefFrameBytes =
        kPlaybackRefFrameSamples * sizeof(int16_t);
inline constexpr int kPlaybackRefStatsIntervalMs = 5000;

}  // namespace btspeaker

#endif
