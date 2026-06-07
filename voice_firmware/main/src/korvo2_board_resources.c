#include "korvo2_board_resources.h"

#include "audio_hal.h"
#include "board.h"
#include "es7210.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "periph_lcd.h"

static const char *kTag = "KORVO2_BOARD";

static audio_board_handle_t g_board_handle = NULL;
static esp_periph_set_handle_t g_periph_set = NULL;
static esp_lcd_panel_handle_t g_lcd_panel = NULL;
static bool g_initialized = false;

esp_err_t korvo2_board_resources_init(void) {
    if (g_initialized) {
        return ESP_OK;
    }

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    g_periph_set = esp_periph_set_init(&periph_cfg);
    if (!g_periph_set) {
        ESP_LOGE(kTag, "Failed to init peripheral set");
        return ESP_ERR_NO_MEM;
    }

    g_board_handle = audio_board_init();
    if (!g_board_handle || !g_board_handle->audio_hal || !g_board_handle->adc_hal) {
        ESP_LOGE(kTag, "Failed to init Korvo-2 audio board");
        return ESP_FAIL;
    }

    // Korvo-2 shares I2S clocks through ES8311. ADF WWE/voip examples never START adc_hal.
    esp_err_t ret = audio_hal_ctrl_codec(g_board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start ES8311 codec: %s", esp_err_to_name(ret));
        return ret;
    }

    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_30DB);

#if defined(FUNC_LCD_SCREEN_EN) && (FUNC_LCD_SCREEN_EN)
    g_lcd_panel = (esp_lcd_panel_handle_t)audio_board_lcd_init(g_periph_set, NULL);
    if (!g_lcd_panel) {
        ESP_LOGE(kTag, "Failed to init Korvo-2 LCD panel");
        return ESP_FAIL;
    }
#endif

    g_initialized = true;
    ESP_LOGI(kTag, "Korvo-2 board resources initialized");
    return ESP_OK;
}

audio_board_handle_t korvo2_board_resources_board(void) {
    return g_board_handle;
}

esp_periph_set_handle_t korvo2_board_resources_periph_set(void) {
    return g_periph_set;
}

esp_lcd_panel_handle_t korvo2_board_resources_lcd_panel(void) {
    return g_lcd_panel;
}
