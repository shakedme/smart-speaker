#pragma once

#include "display_backend.hpp"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

class Korvo2LcdDisplayBackend final : public DisplayBackend {
public:
    ~Korvo2LcdDisplayBackend() override;

    const char *name() const override { return "korvo2_lcd"; }
    esp_err_t init() override;
    DisplaySize size() const override;
    uint16_t *framebuffer() override;
    uint16_t framebufferWidth() const override;
    uint16_t framebufferHeight() const override;
    void clear() override;
    void drawText(uint16_t x, uint16_t y, const char *text) override;
    void drawWaveform(const float *buckets, size_t bucket_count) override;
    void flush() override;
    bool beginDraw() override;
    void endDraw() override;

private:
    static constexpr uint16_t kWidth = 320;
    static constexpr uint16_t kHeight = 240;
    static constexpr int kFlushLines = 16;

    uint16_t *framebuffer_ = nullptr;
    SemaphoreHandle_t drawMutex_ = nullptr;
};

}  // namespace btspeaker
