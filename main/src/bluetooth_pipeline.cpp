#include "../include/bluetooth_pipeline.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include "bluetooth_service.h"
#include "freertos/FreeRTOS.h"
}

namespace btspeaker {

    BluetoothPipeline::BluetoothPipeline() : BaseAudioPipeline(TAG) {}

    void BluetoothPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up BT pipeline");

        bluetooth_service_cfg_t btCfg = {
                .device_name = "ESP32_SPEAKER",
                .remote_name = nullptr,
                .mode = BLUETOOTH_A2DP_SINK,
                .user_callback = {}
        };
        esp_err_t ret = bluetooth_service_start(&btCfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Bluetooth service: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to start Bluetooth service");
        }
        btServiceStarted = true;
        ESP_LOGI(TAG, "Bluetooth A2DP Sink started, device name: %s", btCfg.device_name);

        audio_element_handle_t bt_handle = bluetooth_service_create_stream();
        if (!bt_handle) {
            ESP_LOGE(TAG, "Failed to create Bluetooth stream");
            throw std::runtime_error("Failed to create Bluetooth stream");
        }

        audioPipeline = AudioPipelineUtil::createAudioPipeline(AudioSource::BLUETOOTH,
                                                               bt_handle);

    }

    void BluetoothPipeline::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up audio pipeline");
        AudioPipelineUtil::cleanupPipeline(*audioPipeline);
        audioPipeline.reset();

        if (btServiceStarted) {
            bluetooth_service_destroy();
            btServiceStarted = false;
            ESP_LOGI(TAG, "Bluetooth service stopped");
        }

        ESP_LOGI(TAG, "Bluetooth pipeline cleanup complete");
    }

} // namespace btspeaker 