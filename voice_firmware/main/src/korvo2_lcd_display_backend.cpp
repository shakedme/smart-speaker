#include "korvo2_lcd_display_backend.hpp"
#include "korvo2_board_resources.hpp"

extern "C" {
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
}

#include <cstring>

namespace btspeaker {

namespace {
constexpr const char *kTag = "KORVO2_LCD";
constexpr uint16_t kBgColor = 0x0000;
}  // namespace

Korvo2LcdDisplayBackend::~Korvo2LcdDisplayBackend() {
    if (drawMutex_) {
        vSemaphoreDelete(drawMutex_);
        drawMutex_ = nullptr;
    }
    if (framebuffer_) {
        heap_caps_free(framebuffer_);
        framebuffer_ = nullptr;
    }
}

esp_err_t Korvo2LcdDisplayBackend::init() {
    esp_err_t ret = Korvo2BoardResources::ensureInitialized();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Board init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!Korvo2BoardResources::lcdPanel()) {
        ESP_LOGE(kTag, "LCD panel handle unavailable");
        return ESP_FAIL;
    }

    if (!drawMutex_) {
        drawMutex_ = xSemaphoreCreateMutex();
        if (!drawMutex_) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!framebuffer_) {
        const size_t bytes = static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);
        framebuffer_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!framebuffer_) {
            framebuffer_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!framebuffer_) {
            ESP_LOGE(kTag, "Failed to allocate LCD framebuffer");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(drawMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        std::memset(framebuffer_, 0, static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t));
        xSemaphoreGive(drawMutex_);
    }

    ESP_LOGI(kTag, "Korvo-2 LCD backend initialized (%ux%u)", kWidth, kHeight);
    return ESP_OK;
}

DisplaySize Korvo2LcdDisplayBackend::size() const {
    return {.width = kWidth, .height = kHeight};
}

uint16_t *Korvo2LcdDisplayBackend::framebuffer() {
    return framebuffer_;
}

uint16_t Korvo2LcdDisplayBackend::framebufferWidth() const {
    return kWidth;
}

uint16_t Korvo2LcdDisplayBackend::framebufferHeight() const {
    return kHeight;
}

void Korvo2LcdDisplayBackend::clear() {
    if (!framebuffer_ || !beginDraw()) {
        return;
    }
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
        framebuffer_[i] = kBgColor;
    }
    xSemaphoreGive(drawMutex_);
}

void Korvo2LcdDisplayBackend::drawText(uint16_t, uint16_t, const char *) {}

void Korvo2LcdDisplayBackend::drawWaveform(const float *, size_t) {}

bool Korvo2LcdDisplayBackend::beginDraw() {
    if (!drawMutex_) {
        return true;
    }
    return xSemaphoreTake(drawMutex_, pdMS_TO_TICKS(250)) == pdTRUE;
}

void Korvo2LcdDisplayBackend::endDraw() {
    flush();
}

void Korvo2LcdDisplayBackend::flush() {
    esp_lcd_panel_handle_t panel = Korvo2BoardResources::lcdPanel();
    if (!panel || !framebuffer_) {
        if (drawMutex_) {
            xSemaphoreGive(drawMutex_);
        }
        return;
    }

    for (int y = 0; y < kHeight; y += kFlushLines) {
        const int lineCount = (y + kFlushLines <= kHeight) ? kFlushLines : (kHeight - y);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(
                panel,
                0,
                y,
                kWidth,
                y + lineCount,
                framebuffer_ + static_cast<size_t>(y) * kWidth);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "draw_bitmap failed at y=%d: %s", y, esp_err_to_name(ret));
            break;
        }
    }

    if (drawMutex_) {
        xSemaphoreGive(drawMutex_);
    }
}

}  // namespace btspeaker
