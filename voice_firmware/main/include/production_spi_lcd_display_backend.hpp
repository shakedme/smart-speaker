#pragma once

#include "display_backend.hpp"

namespace btspeaker {

class ProductionSpiLcdDisplayBackend final : public DisplayBackend {
public:
    const char *name() const override;
    esp_err_t init() override;
    DisplaySize size() const override;
    void clear() override;
    void drawText(uint16_t x, uint16_t y, const char *text) override;
    void drawWaveform(const float *buckets, size_t bucket_count) override;
    void flush() override;
};

}  // namespace btspeaker
