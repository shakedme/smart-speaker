#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "esp_err.h"
}

namespace btspeaker {

struct DisplaySize {
    uint16_t width;
    uint16_t height;
};

class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;
    virtual const char *name() const = 0;
    virtual esp_err_t init() = 0;
    virtual DisplaySize size() const = 0;
    virtual void clear() = 0;
    virtual void drawText(uint16_t x, uint16_t y, const char *text) = 0;
    virtual void drawWaveform(const float *buckets, size_t bucket_count) = 0;
    virtual void flush() = 0;

    virtual uint16_t *framebuffer() { return nullptr; }
    virtual uint16_t framebufferWidth() const { return 0; }
    virtual uint16_t framebufferHeight() const { return 0; }
    virtual bool beginDraw() { return true; }
    virtual void endDraw() { flush(); }
};

}  // namespace btspeaker
