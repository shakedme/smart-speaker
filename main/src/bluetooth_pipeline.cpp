#include "../include/bluetooth_pipeline.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include "bluetooth_service.h"
#include "freertos/FreeRTOS.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
}

namespace btspeaker {

    BluetoothPipeline *BluetoothPipeline::instance = nullptr;
    static uint8_t avrcp_tl = 0; // Transaction label for AVRCP commands

    BluetoothPipeline::BluetoothPipeline()
            : BaseAudioPipeline(TAG),
              btConnected(false) {
        instance = this;

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
    }

    void BluetoothPipeline::bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p_param) {
        auto *self = instance;
        if (!self) return;

        switch (event) {
            case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
                if (p_param->conn_stat.connected) {
                    memcpy(self->connectedDeviceAddr, p_param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
                    self->btConnected = true;
                    ESP_LOGI("BluetoothService", "Bluetooth connected");
                    if (self->connectedCallback) {
                        self->connectedCallback();
                    }
                } else {
                    self->btConnected = false;
                    memset(self->connectedDeviceAddr, 0, ESP_BD_ADDR_LEN);
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

        audio_element_handle_t bt_handle = bluetooth_service_create_stream();
        if (!bt_handle) {
            ESP_LOGE(TAG, "Failed to create Bluetooth stream");
            throw std::runtime_error("Failed to create Bluetooth stream");
        }

        audioPipeline = AudioPipelineUtil::createAudioPipeline(AudioSource::BLUETOOTH,
                                                               bt_handle);

        start();
    }

    void BluetoothPipeline::cleanupPipeline() {
        if (!isRunning()) {
            return;
        }
        ESP_LOGI(TAG, "Cleaning up audio pipeline");
        AudioPipelineUtil::cleanupPipeline(*audioPipeline);
        audioPipeline.reset();
        running = false;
        ESP_LOGI(TAG, "Bluetooth pipeline cleanup complete");
    }

    void BluetoothPipeline::disconnectCurrentDevice() {
        if (btConnected) {
            esp_err_t ret = esp_a2d_sink_disconnect(connectedDeviceAddr);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Forced BT disconnection initiated");
            } else {
                ESP_LOGE(TAG, "Failed to disconnect BT device: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "No BT device connected to disconnect");
        }
    }

    // Helper to send passthrough command (pressed + released)
    static void send_avrcp_passthrough_cmd(uint8_t key_code) {
        uint8_t tl = avrcp_tl++ & 0x0F;
        esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }

    void BluetoothPipeline::avrcpPause() const {
        if (btConnected)
            send_avrcp_passthrough_cmd(ESP_AVRC_PT_CMD_PAUSE);
    }

    void BluetoothPipeline::avrcpResume() const {
        if (btConnected)
            send_avrcp_passthrough_cmd(ESP_AVRC_PT_CMD_PLAY);
    }

    void BluetoothPipeline::avrcpNext() const {
        if (btConnected)
            send_avrcp_passthrough_cmd(ESP_AVRC_PT_CMD_FORWARD);
    }

    void BluetoothPipeline::avrcpPrevious() const {
        if (btConnected)
            send_avrcp_passthrough_cmd(ESP_AVRC_PT_CMD_BACKWARD);
    }

    void BluetoothPipeline::setAvrcpVolume(uint8_t volume) const {
        if (btConnected) {
            uint8_t tl = avrcp_tl++ & 0x0F;
            esp_avrc_ct_send_set_absolute_volume_cmd(tl, volume > 0x7F ? 0x7F : volume);
        }
    }

} // namespace btspeaker
