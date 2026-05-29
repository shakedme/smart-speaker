#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

struct ImaAdpcmState {
    int16_t predictor = 0;
    int8_t index = 0;
};

// Encodes 16-bit PCM samples to IMA ADPCM (4 bits/sample).
// Returns number of output bytes written, or 0 on error.
size_t imaAdpcmEncode(const int16_t *samples,
                      size_t sample_count,
                      uint8_t *out,
                      size_t out_capacity,
                      ImaAdpcmState &state);

// Decodes IMA ADPCM to 16-bit PCM (used by backend tests / optional device self-test).
size_t imaAdpcmDecode(const uint8_t *adpcm,
                      size_t adpcm_bytes,
                      int16_t *samples_out,
                      size_t samples_capacity,
                      ImaAdpcmState &state);

}  // namespace btspeaker
