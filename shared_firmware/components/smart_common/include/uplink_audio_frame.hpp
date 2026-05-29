#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

enum class AudioFrameType : uint8_t {
    Mic = 1,
    PlaybackRef = 2,
};

enum class UplinkSampleFormat : uint8_t {
    Int16Pcm = 1,
    ImaAdpcmBlock = 2,
};

#pragma pack(push, 1)
struct UplinkAudioFrameHeader {
    uint8_t frame_type;
    uint8_t sample_format;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint32_t sequence;
    int64_t timestamp_us;
    uint32_t num_samples;
};
#pragma pack(pop)

static_assert(sizeof(UplinkAudioFrameHeader) == 24, "UplinkAudioFrameHeader size mismatch");

inline constexpr size_t kUplinkFramedHeaderSize = sizeof(UplinkAudioFrameHeader);

inline size_t uplinkFramedPacketSize(uint32_t num_samples, uint16_t channel_count) {
    return 1 + kUplinkFramedHeaderSize +
           static_cast<size_t>(num_samples) * channel_count * sizeof(int16_t);
}

}  // namespace btspeaker
