#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

using MicAudioCallback = std::function<void(const int16_t *samples, size_t sample_count, int64_t timestamp_us)>;

class MicCaptureBackend {
public:
    virtual ~MicCaptureBackend() = default;
    virtual const char *name() const = 0;
    virtual esp_err_t init() = 0;
    virtual esp_err_t start(MicAudioCallback callback) = 0;
    virtual void stop() = 0;
};

}  // namespace btspeaker
