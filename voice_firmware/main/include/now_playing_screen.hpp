#pragma once

#include <cstddef>
#include <cstdint>

namespace btspeaker {

struct NowPlayingUiState;

class NowPlayingScreen {
public:
    static constexpr uint8_t kFontScale = 2;
    static constexpr uint8_t kGlyphWidth = 8;
    static constexpr uint8_t kGlyphHeight = 8;
    static constexpr uint16_t kCharWidth = kGlyphWidth * kFontScale;
    static constexpr uint16_t kCharHeight = kGlyphHeight * kFontScale;
    static constexpr uint16_t kLineGap = 8;

    static void truncateCopy(char *dst, size_t dst_len, const char *src);
    static uint16_t measureTextWidth(const char *text);
    static void renderText(uint16_t *framebuffer,
                           uint16_t width,
                           uint16_t height,
                           uint16_t x,
                           uint16_t y,
                           const char *text,
                           uint16_t color);
    static void renderTextCentered(uint16_t *framebuffer,
                                   uint16_t width,
                                   uint16_t height,
                                   uint16_t center_y,
                                   const char *text,
                                   uint16_t color);
    static void renderTextCenteredPair(uint16_t *framebuffer,
                                       uint16_t width,
                                       uint16_t height,
                                       uint16_t center_y,
                                       const char *line1,
                                       const char *line2,
                                       uint16_t color1,
                                       uint16_t color2);
    static void renderMetadata(uint16_t *framebuffer,
                               uint16_t width,
                               uint16_t height,
                               const NowPlayingUiState &state,
                               bool connecting,
                               bool hasMetadata);
};

}  // namespace btspeaker
