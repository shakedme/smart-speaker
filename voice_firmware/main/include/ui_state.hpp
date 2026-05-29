#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace btspeaker {

struct NowPlayingUiState {
    char source[16] = "unknown";
    char playback_state[16] = "idle";
    char title[96] = "";
    char artist[96] = "";
    char album[96] = "";
    uint32_t duration_ms = 0;
    uint32_t position_ms = 0;
    int64_t updated_at_ms = 0;
};

struct WaveformUiState {
    static constexpr size_t kMaxBuckets = 32;
    std::array<float, kMaxBuckets> buckets{};
    size_t bucket_count = 0;
    int64_t updated_at_ms = 0;
};

}  // namespace btspeaker
