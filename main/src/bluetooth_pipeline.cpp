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

    BluetoothPipeline *BluetoothPipeline::instance = nullptr;

    BluetoothPipeline::BluetoothPipeline() : BaseAudioPipeline(TAG) {
        instance = this;
    }

    void BluetoothPipeline::bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p_param) {
        esp_avrc_ct_cb_param_t *rc = p_param;
        auto *self = instance;
        switch (event) {
            case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
                if (rc->conn_stat.connected) {
                    ESP_LOGI("BluetoothService", "Bluetooth connected - available mem: %d bytes",
                             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                    if (self->connectedCallback) {
                        self->connectedCallback();
                    }
                } else {
                    ESP_LOGI("BluetoothService", "Bluetooth disconnected");
                }
                break;
            }
            default:
                break;
        }
    }

    void BluetoothPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up BT pipeline");

        bluetooth_service_cfg_t btCfg = {
                .device_name = "ESP32_SPEAKER",
                .remote_name = nullptr,
                .mode = BLUETOOTH_A2DP_SINK,
                .user_callback = {
                        .user_avrc_ct_cb = bt_app_avrc_ct_cb,
                }
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