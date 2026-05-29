#pragma once

// I2S pin mapping for ESP32-DevKitC-32E → PCM5102A DAC → TPA3116D2 Amplifier
// Audio chain: ESP32 I2S → PCM5102A (Digital to Analog) → 3.5mm → TPA3116D2 → Speakers
#define I2S_NUM         (0)
#define I2S_BCK_IO      (25) // GPIO25 → PCM5102A BCK (Bit Clock)
#define I2S_WS_IO       (22) // GPIO22 → PCM5102A LCK (Word Select / L-R Clock)  
#define I2S_DO_IO       (26) // GPIO26 → PCM5102A DIN (Data Input)
#define I2S_DI_IO       (-1) // Not used (DAC output only, no audio input)

// SD card pin definitions (set to -1 if not used)
#define ESP_SD_PIN_CMD   (-1)
#define ESP_SD_PIN_D0    (-1)
#define ESP_SD_PIN_CLK   (-1)
#define ESP_SD_PIN_D3    (-1)

// Board PA gain (default to 0 if not used)
#define BOARD_PA_GAIN 0

// Minimal stubs for ESP-ADF compatibility
static inline void board_init(void) {}

// Provide a stub for get_sdcard_open_file_num_max if not using SD card
static inline int get_sdcard_open_file_num_max(void) { return 5; }

// Stub for get_reset_board_gpio (return -1 if not used)
static inline int get_reset_board_gpio(void) { return -1; }

// Stub for get_pa_enable_gpio (return -1 if not used)
static inline int get_pa_enable_gpio(void) { return -1; }

// Stub for get_es7243_mclk_gpio (return -1 if not used)
static inline int get_es7243_mclk_gpio(void) { return -1; }

// Stub for get_i2c_pins (return 0 for success, do nothing)
static inline int get_i2c_pins(int i2c_num, void *cfg) { return 0; }

// Stub for get_spi_pins (return 0 for success, do nothing)
static inline int get_spi_pins(void *buscfg, void *devcfg) { return 0; } 