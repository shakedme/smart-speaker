#include "../include/ima_adpcm.hpp"

namespace btspeaker {

namespace {

constexpr int8_t kIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
};

constexpr int16_t kStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

inline int16_t clamp16(int32_t v) {
    if (v < -32768) {
        return -32768;
    }
    if (v > 32767) {
        return 32767;
    }
    return static_cast<int16_t>(v);
}

inline uint8_t encodeNibble(int16_t sample, ImaAdpcmState &state) {
    const int step = kStepTable[state.index];
    int diff = static_cast<int>(sample) - static_cast<int>(state.predictor);
    uint8_t nibble = 0;
    int delta = step >> 3;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        delta += step;
    }
    const int half_step = step >> 1;
    if (diff >= half_step) {
        nibble |= 2;
        diff -= half_step;
        delta += half_step;
    }
    const int quarter_step = step >> 2;
    if (diff >= quarter_step) {
        nibble |= 1;
        delta += quarter_step;
    }

    if (nibble & 8) {
        delta = -delta;
    }

    state.predictor = clamp16(static_cast<int32_t>(state.predictor) + delta);
    state.index = static_cast<int8_t>(state.index + kIndexTable[nibble]);
    if (state.index < 0) {
        state.index = 0;
    } else if (state.index > 88) {
        state.index = 88;
    }

    return nibble;
}

inline void decodeNibble(uint8_t nibble, ImaAdpcmState &state) {
    const int step = kStepTable[state.index];
    int delta = step >> 3;
    if (nibble & 4) {
        delta += step;
    }
    if (nibble & 2) {
        delta += step >> 1;
    }
    if (nibble & 1) {
        delta += step >> 2;
    }
    if (nibble & 8) {
        delta = -delta;
    }

    state.predictor = clamp16(static_cast<int32_t>(state.predictor) + delta);
    state.index = static_cast<int8_t>(state.index + kIndexTable[nibble]);
    if (state.index < 0) {
        state.index = 0;
    } else if (state.index > 88) {
        state.index = 88;
    }
}

}  // namespace

size_t imaAdpcmEncode(const int16_t *samples,
                      size_t sample_count,
                      uint8_t *out,
                      size_t out_capacity,
                      ImaAdpcmState &state) {
    if (!samples || !out || sample_count == 0) {
        return 0;
    }

    const size_t needed = (sample_count + 1) / 2;
    if (out_capacity < needed) {
        return 0;
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < sample_count; i += 2) {
        const uint8_t low = encodeNibble(samples[i], state);
        uint8_t high = 0;
        if (i + 1 < sample_count) {
            high = encodeNibble(samples[i + 1], state);
        }
        out[out_idx++] = static_cast<uint8_t>((high << 4) | (low & 0x0F));
    }

    return out_idx;
}

size_t imaAdpcmDecode(const uint8_t *adpcm,
                      size_t adpcm_bytes,
                      int16_t *samples_out,
                      size_t samples_capacity,
                      ImaAdpcmState &state) {
    if (!adpcm || !samples_out || adpcm_bytes == 0) {
        return 0;
    }

    size_t samples_written = 0;
    for (size_t i = 0; i < adpcm_bytes; ++i) {
        if (samples_written >= samples_capacity) {
            break;
        }
        const uint8_t byte = adpcm[i];
        decodeNibble(byte & 0x0F, state);
        samples_out[samples_written++] = state.predictor;
        if (samples_written >= samples_capacity) {
            break;
        }
        decodeNibble((byte >> 4) & 0x0F, state);
        samples_out[samples_written++] = state.predictor;
    }

    return samples_written;
}

}  // namespace btspeaker
