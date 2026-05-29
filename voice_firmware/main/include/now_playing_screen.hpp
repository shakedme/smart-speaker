#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

struct NowPlayingUiState;

class NowPlayingScreen {
public:
    static constexpr uint16_t kTitleY = 108;
    static constexpr uint16_t kArtistY = 136;
    static constexpr uint16_t kStatusY = 164;
    static constexpr uint16_t kMarginX = 8;

    static void truncateCopy(char *dst, size_t dst_len, const char *src);
    static void renderText(uint16_t *framebuffer,
                           uint16_t width,
                           uint16_t height,
                           uint16_t x,
                           uint16_t y,
                           const char *text,
                           uint16_t color);
    static void renderMetadata(uint16_t *framebuffer,
                               uint16_t width,
                               uint16_t height,
                               const NowPlayingUiState &state,
                               bool connecting,
                               bool hasMetadata);
};

}  // namespace btspeaker
