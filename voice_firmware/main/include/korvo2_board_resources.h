#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_peripherals.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_board_handle *audio_board_handle_t;

esp_err_t korvo2_board_resources_init(void);
audio_board_handle_t korvo2_board_resources_board(void);
esp_periph_set_handle_t korvo2_board_resources_periph_set(void);
esp_lcd_panel_handle_t korvo2_board_resources_lcd_panel(void);

#ifdef __cplusplus
}
#endif
