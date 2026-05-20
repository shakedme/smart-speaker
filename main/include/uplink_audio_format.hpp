#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

// WebSocket binary mic uplink framing (first byte of each chunk).
inline constexpr uint8_t kUplinkFormatPcm = 0;
inline constexpr uint8_t kUplinkFormatImaAdpcm = 1;

inline constexpr size_t kUplinkFrameHeaderSize = 1;

}  // namespace btspeaker
