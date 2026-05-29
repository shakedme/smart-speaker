#include "waveform_view.hpp"

#include <algorithm>
#include <cmath>

namespace btspeaker {

namespace {
constexpr uint16_t kColorWave = 0x07E0;
constexpr uint16_t kColorWaveDim = 0x0320;
}  // namespace

void WaveformView::render(uint16_t *framebuffer,
                            const uint16_t width,
                            const uint16_t height,
                            const float *buckets,
                            const size_t bucket_count,
                            const uint16_t color) {
    if (!framebuffer || !buckets || bucket_count == 0) {
        return;
    }

    const uint16_t region_bottom = std::min<uint16_t>(height, kTopY + kRegionHeight);
    const uint16_t drawable_width = width - (2 * kMarginX);
    if (drawable_width == 0) {
        return;
    }

    const size_t bar_count = std::min(bucket_count, static_cast<size_t>(drawable_width / 4));
    if (bar_count == 0) {
        return;
    }

    const uint16_t bar_width = std::max<uint16_t>(2, static_cast<uint16_t>(drawable_width / bar_count));
    const uint16_t gap = bar_width >= 4 ? 1 : 0;

    for (size_t i = 0; i < bar_count; ++i) {
        const size_t bucket_idx = (i * bucket_count) / bar_count;
        const float value = std::clamp(buckets[bucket_idx], 0.0f, 1.0f);
        const uint16_t bar_height = static_cast<uint16_t>(value * (kRegionHeight - 16));
        const uint16_t x0 = kMarginX + static_cast<uint16_t>(i * bar_width);
        const uint16_t y0 = region_bottom - bar_height;

        for (uint16_t y = y0; y < region_bottom; ++y) {
            for (uint16_t x = x0; x < x0 + bar_width - gap && x < width; ++x) {
                framebuffer[static_cast<size_t>(y) * width + x] = value > 0.05f ? color : kColorWaveDim;
            }
        }
    }
}

}  // namespace btspeaker
