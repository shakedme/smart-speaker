#include "PCM5102AudioSink.h"



// Include correct pin definitions from board.h
extern "C" {
#include "board.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

static const char *TAG = "PCM5102AudioSink";

PCM5102AudioSink::PCM5102AudioSink() {
    i2s_config_t i2s_config = {

            .mode = (i2s_mode_t) (I2S_MODE_MASTER | I2S_MODE_TX),  // Only TX
            .sample_rate = 44100,
            .bits_per_sample = (i2s_bits_per_sample_t) 16,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  //2-channels
            .communication_format = (i2s_comm_format_t) I2S_COMM_FORMAT_I2S,
            .intr_alloc_flags = 0,  //Default interrupt priority
            .dma_desc_num = 8,
            .dma_frame_num = 512,
            .use_apll = true,
            .tx_desc_auto_clear = true,  //Auto clear tx descriptor on underflow
            .fixed_mclk = 384 * 44100
    };

    // Use correct GPIO pins from board.h to match hardware setup
    i2s_pin_config_t pin_config = {
            .bck_io_num = I2S_BCK_IO,    // GPIO25: Bit Clock
            .ws_io_num = I2S_WS_IO,      // GPIO22: Word Select (LCK)
            .data_out_num = I2S_DO_IO,   // GPIO26: Data Out (DIN)
            .data_in_num = -1            // Not used (DAC only)
    };

    // Log I2S driver installation for debugging
    printf("PCM5102AudioSink: Installing I2S driver\n");
    esp_err_t ret = i2s_driver_install((i2s_port_t) 0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        printf("PCM5102AudioSink: I2S driver install failed: %s\n", esp_err_to_name(ret));
    } else {
        printf("PCM5102AudioSink: I2S driver installed successfully\n");
    }

    i2s_set_pin((i2s_port_t) 0, &pin_config);

    startI2sFeed();
}

PCM5102AudioSink::~PCM5102AudioSink() {
    printf("PCM5102AudioSink: Destructor called, cleaning up I2S\n");

    // Stop I2S stream first
    i2s_stop((i2s_port_t) 0);
    i2s_zero_dma_buffer((i2s_port_t) 0);

    // Wait for I2S operations to complete
    vTaskDelay(pdMS_TO_TICKS(50));

    // CRITICAL: Uninstall I2S driver to release hardware for other users
    esp_err_t ret = i2s_driver_uninstall((i2s_port_t) 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCM5102AudioSink: Warning - I2S driver uninstall failed: (%s)\n", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "PCM5102AudioSink: I2S driver uninstalled successfully\n");
    }

    // Additional delay to ensure hardware is fully released
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "PCM5102AudioSink: Complete I2S cleanup finished\n");
}
