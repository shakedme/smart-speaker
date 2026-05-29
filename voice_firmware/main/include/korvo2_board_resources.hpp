#pragma once

extern "C" {
#include "korvo2_board_resources.h"
}

namespace btspeaker {

class Korvo2BoardResources {
public:
    static esp_err_t ensureInitialized() { return korvo2_board_resources_init(); }
    static audio_board_handle_t boardHandle() { return korvo2_board_resources_board(); }
    static esp_periph_set_handle_t periphSet() { return korvo2_board_resources_periph_set(); }
    static esp_lcd_panel_handle_t lcdPanel() { return korvo2_board_resources_lcd_panel(); }
};

}  // namespace btspeaker
