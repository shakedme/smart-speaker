#include "waveform_view.hpp"

#include "lcd_framebuffer.hpp"
#include <algorithm>

namespace btspeaker {

namespace {
constexpr uint16_t kDimColor = 0x04A0;
constexpr uint16_t kFillColor = 0x030C;
constexpr uint16_t kColumnStepPx = 4;
constexpr float kInputGain = 4.0f;
constexpr float kActiveFloor = 0.004f;

uint16_t bucketToAmplitudePx(const float value, const uint16_t max_amp) {
    const float boosted = std::clamp(value * kInputGain, 0.0f, 1.0f);
    const uint16_t active_floor_px = value > kActiveFloor ? 8U : 2U;
    return static_cast<uint16_t>(active_floor_px + static_cast<uint16_t>(boosted * static_cast<float>(max_amp)));
}
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

    const uint16_t region_top = kTopY;
    const uint16_t region_bottom = std::min<uint16_t>(height, kTopY + kRegionHeight);
    if (region_top >= region_bottom) {
        return;
    }
    const uint16_t drawable_width = width - (2 * kMarginX);
    if (drawable_width < 2) {
        return;
    }

    const uint16_t center_y = static_cast<uint16_t>(region_top + (region_bottom - region_top) / 2U);
    const uint16_t max_amp = static_cast<uint16_t>((region_bottom - region_top) / 2U - 4U);
    uint16_t previous_amp = 2;

    for (uint16_t x = kMarginX; x < width - kMarginX; x += kColumnStepPx) {
        const size_t bucket_idx = std::min(
                bucket_count - 1,
                (static_cast<size_t>(x - kMarginX) * bucket_count) / std::max<uint16_t>(1, drawable_width));
        const uint16_t target_amp = bucketToAmplitudePx(buckets[bucket_idx], max_amp);
        const uint16_t amp = static_cast<uint16_t>((previous_amp * 3U + target_amp) / 4U);
        previous_amp = amp;

        const uint16_t top = center_y > amp ? static_cast<uint16_t>(center_y - amp) : region_top;
        const uint16_t bottom = std::min<uint16_t>(region_bottom - 1U, static_cast<uint16_t>(center_y + amp));

        for (uint16_t y = top; y <= bottom; ++y) {
            const bool edge = y == top || y == bottom ||
                              y == static_cast<uint16_t>(center_y - 1U) ||
                              y == center_y ||
                              y == static_cast<uint16_t>(center_y + 1U);
            const uint16_t draw_color = edge ? color : kFillColor;
            for (uint16_t dx = 0; dx < kColumnStepPx - 1U && x + dx < width - kMarginX; ++dx) {
                setFramebufferPixel(framebuffer, width, height, static_cast<uint16_t>(x + dx), y, draw_color);
            }
        }

        for (uint16_t dx = 0; dx < kColumnStepPx - 1U && x + dx < width - kMarginX; ++dx) {
            if (amp <= 2U) {
                setFramebufferPixel(framebuffer, width, height, static_cast<uint16_t>(x + dx), center_y, kDimColor);
            }
        }
    }
}

}  // namespace btspeaker
