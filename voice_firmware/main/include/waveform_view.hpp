#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

class WaveformView {
public:
    static constexpr uint16_t kRegionHeight = 96;
    static constexpr uint16_t kMarginX = 8;
    static constexpr uint16_t kTopY = 8;

    static void render(uint16_t *framebuffer,
                       uint16_t width,
                       uint16_t height,
                       const float *buckets,
                       size_t bucket_count,
                       uint16_t color);
};

}  // namespace btspeaker
