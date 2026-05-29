/*
 * Board pin configuration implementation for ESP32-DevKitC-32E
 */

#include "board_pins_config.h"
#include "driver/i2c.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_err.h"

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    // I2C is not used in our setup since GPIO22 is used for I2S WS
    // Return success but don't modify the config
    (void)port;
    (void)i2c_config;
    return ESP_OK;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    if (i2s_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Configure I2S pins for PCM5102A DAC
    i2s_config->mck_io_num = BOARD_I2S_MCK_PIN;       // -1 (not used)
    i2s_config->bck_io_num = BOARD_I2S_BCK_PIN;       // GPIO25
    i2s_config->ws_io_num = BOARD_I2S_WS_PIN;         // GPIO22
    i2s_config->data_out_num = BOARD_I2S_DATA_OUT_PIN; // GPIO26
    i2s_config->data_in_num = BOARD_I2S_DATA_IN_PIN;  // -1 (not used)

    return ESP_OK;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config)
{
    // SPI is not used in our current setup
    // Return success but don't modify the configs
    (void)spi_config;
    (void)spi_device_interface_config;
    return ESP_OK;
} 