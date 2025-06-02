/*
 * Board pin configuration for ESP32-DevKitC-32E with PCM5102A and MAX9814
 *
 * Hardware setup:
 * - ESP32-DevKitC-32E (ESP32-WROOM-32E module)
 * - PCM5102A I2S DAC Module
 * - MAX9814 Microphone Module
 * - TPA3116D2 Amplifier (Sure Electronics AA-JA32172)
 */

#ifndef _BOARD_PINS_CONFIG_H_
#define _BOARD_PINS_CONFIG_H_

#include "driver/i2c.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board i2s pin definition
 */
typedef struct {
    int mck_io_num;         /*!< MCK pin, output */
    int bck_io_num;         /*!< BCK pin, input in slave role, output in master role */
    int ws_io_num;          /*!< WS pin, input in slave role, output in master role */
    int data_out_num;       /*!< DATA pin, output */
    int data_in_num;        /*!< DATA pin, input */
} board_i2s_pin_t;

// I2S Pin Definitions for PCM5102A
#define BOARD_I2S_BCK_PIN       25  // GPIO25: Bit Clock
#define BOARD_I2S_WS_PIN        22  // GPIO22: Word Select (LCK)
#define BOARD_I2S_DATA_OUT_PIN  26  // GPIO26: Data Out (DIN)
#define BOARD_I2S_DATA_IN_PIN   (-1) // Not used for PCM5102A (DAC only)
#define BOARD_I2S_MCK_PIN       (-1) // Not used

// I2C Pin Definitions (not used in current setup, but required for compatibility)
#define BOARD_I2C_SDA_PIN       21  // Standard ESP32 I2C SDA
#define BOARD_I2C_SCL_PIN       22  // Standard ESP32 I2C SCL (conflicts with I2S WS)
// Note: Since we're using GPIO22 for I2S WS, I2C is disabled

// Function implementations

/**
 * @brief Get i2c pins configuration
 */
esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config);

/**
 * @brief Get i2s pins configuration
 */
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config);

/**
 * @brief Get spi pins configuration
 */
esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config);

// All the following functions return -1 (non-existent) for ESP32-DevKitC-32E
// since it's a generic development board without these specialized features

static inline int8_t get_sdcard_intr_gpio(void) { return -1; }
static inline int8_t get_sdcard_open_file_num_max(void) { return 5; }
static inline int8_t get_sdcard_power_ctrl_gpio(void) { return -1; }
static inline int8_t get_auxin_detect_gpio(void) { return -1; }
static inline int8_t get_headphone_detect_gpio(void) { return -1; }
static inline int8_t get_pa_enable_gpio(void) { return -1; }
static inline int8_t get_adc_detect_gpio(void) { return -1; }
static inline int8_t get_es7243_mclk_gpio(void) { return -1; }
static inline int8_t get_input_rec_id(void) { return -1; }
static inline int8_t get_input_mode_id(void) { return -1; }
static inline int8_t get_input_set_id(void) { return -1; }
static inline int8_t get_input_play_id(void) { return -1; }
static inline int8_t get_input_volup_id(void) { return -1; }
static inline int8_t get_input_voldown_id(void) { return -1; }
static inline int8_t get_reset_codec_gpio(void) { return -1; }
static inline int8_t get_reset_board_gpio(void) { return -1; }
static inline int8_t get_green_led_gpio(void) { return -1; }
static inline int8_t get_blue_led_gpio(void) { return -1; }

#ifdef __cplusplus
}
#endif

#endif /* _BOARD_PINS_CONFIG_H_ */ 