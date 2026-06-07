#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

// Centralize framebuffer writes so text and waveform rendering use identical bounds checks.
inline void setFramebufferPixel(uint16_t *framebuffer,
                                const uint16_t width,
                                const uint16_t height,
                                const uint16_t x,
                                const uint16_t y,
                                const uint16_t color) {
    if (!framebuffer || x >= width || y >= height) {
        return;
    }
    framebuffer[static_cast<size_t>(y) * width + x] = color;
}

}  // namespace btspeaker
