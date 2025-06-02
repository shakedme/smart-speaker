#include <memory>
#include <stdexcept>
#include <iostream>

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_event.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "bluetooth_service.h"
}

namespace btspeaker {

constexpr int I2S_BCK_IO = 25;
constexpr int I2S_WS_IO  = 22;
constexpr int I2S_DO_IO  = 26;

class BluetoothAudioApp {
public:
    BluetoothAudioApp() = default;
    ~BluetoothAudioApp() { cleanup(); }

    void init() {
        // Initialize NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        ESP_LOGI(TAG, "BluetoothAudioApp initialized");
    }

    void run() {
        ESP_LOGI(TAG, "Starting Bluetooth Audio App");
        
        // Create audio event interface
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        evt = audio_event_iface_init(&evt_cfg);
        
        // Bluetooth Service (A2DP Sink)
        bluetooth_service_cfg_t bt_cfg = {
            .device_name = "ESP32_SPEAKER",
            .mode = BLUETOOTH_A2DP_SINK,
        };
        bluetooth_service_start(&bt_cfg);
        ESP_LOGI(TAG, "Bluetooth A2DP Sink started, device name: %s", bt_cfg.device_name);


        // I2S Stream Writer
        i2s_stream_cfg_t i2s_cfg = {
            .type = AUDIO_STREAM_WRITER,
            .transmit_mode = I2S_COMM_MODE_STD,
            .chan_cfg = {
                .id = I2S_NUM_0,
                .role = I2S_ROLE_MASTER,
                .dma_desc_num = 3,
                .dma_frame_num = 312,
                .auto_clear = true,
            },
            .std_cfg = {
                .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = {
                    .mclk = GPIO_NUM_NC,
                    .bclk = GPIO_NUM_25,
                    .ws   = GPIO_NUM_22,
                    .dout = GPIO_NUM_26,
                    .din  = GPIO_NUM_NC,
                },
            },
            .use_alc = false,
            .volume = 0,
            .out_rb_size = 8 * 1024,
            .task_stack = 3584,
            .task_core = 0,
            .task_prio = 23,
            .stack_in_ext = false,
            .multi_out_num = 0,
            .uninstall_drv = true,
            .need_expand = false,
            .expand_src_bits = I2S_DATA_BIT_WIDTH_16BIT,
            .buffer_len = 3600,
        };
        i2s_stream_writer = i2s_stream_init(&i2s_cfg);
        bt_stream_reader = bluetooth_service_create_stream();

        audio_pipeline_cfg_t pipeline_cfg = {
            .rb_size = 8 * 1024,
        };
        pipeline = audio_pipeline_init(&pipeline_cfg);
        audio_pipeline_register(pipeline, bt_stream_reader, "bt");
        audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");
        audio_pipeline_link(pipeline, (const char *[]) {"bt", "i2s"}, 2);

        // Set the event listener for the pipeline
        audio_pipeline_set_listener(pipeline, evt);

        audio_pipeline_run(pipeline);
        ESP_LOGI(TAG, "Audio pipeline started");

        while (true) {
            audio_event_iface_msg_t msg;
            esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Event listen error: %d", ret);
                break;
            }
            
            // Handle Bluetooth music info updates
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)bt_stream_reader
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(bt_stream_reader, &music_info);
                ESP_LOGI(TAG, "Receive music info from Bluetooth, sample_rates=%d, bits=%d, ch=%d",
                         music_info.sample_rates, music_info.bits, music_info.channels);
                // Update I2S stream clock settings
                i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                continue;
            }
            
            // Handle stop events
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                int state = (int)msg.data;
                if (state == AEL_STATUS_STATE_STOPPED || state == AEL_STATUS_STATE_FINISHED) {
                    ESP_LOGW(TAG, "I2S stream stopped or finished");
                    break;
                }
            }
        }
    }

    void cleanup() {
        ESP_LOGI(TAG, "Cleaning up BluetoothAudioApp");
        if (pipeline) {
            audio_pipeline_terminate(pipeline);
            if (i2s_stream_writer) audio_pipeline_unregister(pipeline, i2s_stream_writer);
            if (bt_stream_reader) audio_pipeline_unregister(pipeline, bt_stream_reader);
            audio_pipeline_deinit(pipeline);
            pipeline = nullptr;
        }
        if (i2s_stream_writer) {
            audio_element_deinit(i2s_stream_writer);
            i2s_stream_writer = nullptr;
        }
        if (bt_stream_reader) {
            audio_element_deinit(bt_stream_reader);
            bt_stream_reader = nullptr;
        }
        if (evt) {
            audio_event_iface_destroy(evt);
            evt = nullptr;
        }
        bluetooth_service_destroy();
        ESP_LOGI(TAG, "Bluetooth Audio App stopped");
    }

private:
    static constexpr const char* TAG = "BT_AUDIO";
    audio_pipeline_handle_t pipeline = nullptr;
    audio_element_handle_t i2s_stream_writer = nullptr;
    audio_element_handle_t bt_stream_reader = nullptr;
    audio_event_iface_handle_t evt = nullptr;
};

} // namespace btspeaker