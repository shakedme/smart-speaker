#include "production_spi_lcd_display_backend.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

const char *ProductionSpiLcdDisplayBackend::name() const {
    return "production_spi_lcd";
}

esp_err_t ProductionSpiLcdDisplayBackend::init() {
    ESP_LOGI("UI", "production SPI LCD backend available for future profile");
    return ESP_OK;
}

DisplaySize ProductionSpiLcdDisplayBackend::size() const {
    return {.width = 320, .height = 240};
}

void ProductionSpiLcdDisplayBackend::clear() {}
void ProductionSpiLcdDisplayBackend::drawText(uint16_t, uint16_t, const char *) {}
void ProductionSpiLcdDisplayBackend::drawWaveform(const float *, size_t) {}
void ProductionSpiLcdDisplayBackend::flush() {}

}  // namespace btspeaker
