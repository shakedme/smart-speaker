#include <memory>
#include <stdexcept>
#include <iostream>
#include <cstring>

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
#include "esp_avrc_api.h"
#include "esp_bt.h"
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
        
        // Initialize AVRCP Target for remote control
        ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrcp_tg_callback));
        ESP_ERROR_CHECK(esp_avrc_tg_init());
        
        ESP_LOGI(TAG, "BluetoothAudioApp initialized with AVRCP support");
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

        // Configure AVRCP supported commands
        configure_avrcp_target();

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
        
        // Cleanup AVRCP
        esp_avrc_tg_deinit();
        
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
    static esp_avrc_playback_stat_t current_playback_state;
    static BluetoothAudioApp* instance;

    // Static AVRCP Target callback
    static void avrcp_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
        if (!instance) return;
        
        switch (event) {
            case ESP_AVRC_TG_CONNECTION_STATE_EVT:
                if (param->conn_stat.connected) {
                    ESP_LOGI(TAG, "AVRCP Target connected");
                } else {
                    ESP_LOGI(TAG, "AVRCP Target disconnected");
                }
                break;
                
            case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
                instance->handle_avrcp_passthrough(param->psth_cmd.key_code, param->psth_cmd.key_state);
                break;
                
            case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
                ESP_LOGI(TAG, "AVRCP volume command: %d", param->set_abs_vol.volume);
                // Volume control can be implemented here if needed
                break;
                
            case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
                instance->handle_avrcp_notification_registration(param->reg_ntf.event_id);
                break;
                
            default:
                ESP_LOGD(TAG, "AVRCP TG event: %d", event);
                break;
        }
    }

    void configure_avrcp_target() {
        // Set static instance pointer for callback access
        instance = this;
        
        // Configure supported passthrough commands
        esp_avrc_psth_bit_mask_t cmd_set;
        memset(&cmd_set, 0, sizeof(cmd_set));
        
        // Enable support for essential playback controls
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_STOP);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FAST_FORWARD);
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_REWIND);
        
        esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);
        
        // Configure notification capabilities
        esp_avrc_rn_evt_cap_mask_t evt_set;
        memset(&evt_set, 0, sizeof(evt_set));
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
        esp_avrc_tg_set_rn_evt_cap(&evt_set);
        
        current_playback_state = ESP_AVRC_PLAYBACK_STOPPED;
        ESP_LOGI(TAG, "AVRCP Target configured with playback controls");
    }

    void handle_avrcp_passthrough(uint8_t key_code, uint8_t key_state) {
        if (key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
            return; // Only handle key press events
        }
        
        ESP_LOGI(TAG, "AVRCP passthrough command: %s", get_passthrough_cmd_name(key_code));
        
        switch (key_code) {
            case ESP_AVRC_PT_CMD_PLAY:
                audio_pipeline_resume(pipeline);
                set_playback_state(ESP_AVRC_PLAYBACK_PLAYING);
                break;
                
            case ESP_AVRC_PT_CMD_PAUSE:
                audio_pipeline_pause(pipeline);
                set_playback_state(ESP_AVRC_PLAYBACK_PAUSED);
                break;
                
            case ESP_AVRC_PT_CMD_STOP:
                audio_pipeline_stop(pipeline);
                set_playback_state(ESP_AVRC_PLAYBACK_STOPPED);
                break;
                
            case ESP_AVRC_PT_CMD_FORWARD:
            case ESP_AVRC_PT_CMD_BACKWARD:
                ESP_LOGI(TAG, "Track navigation not implemented for this media source");
                break;
                
            case ESP_AVRC_PT_CMD_FAST_FORWARD:
            case ESP_AVRC_PT_CMD_REWIND:
                ESP_LOGI(TAG, "Seek operations not implemented for this media source");
                break;
                
            default:
                ESP_LOGW(TAG, "Unhandled AVRCP command: 0x%02x", key_code);
                break;
        }
    }

    void handle_avrcp_notification_registration(uint8_t event_id) {
        ESP_LOGI(TAG, "AVRCP notification registration for event: %d", event_id);
        
        if (event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            // Send current playback status
            esp_avrc_rn_param_t rn_param;
            rn_param.playback = current_playback_state;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
    }

    void set_playback_state(esp_avrc_playback_stat_t new_state) {
        if (current_playback_state != new_state) {
            current_playback_state = new_state;
            
            // Notify AVRCP controller about state change
            esp_avrc_rn_param_t rn_param;
            rn_param.playback = current_playback_state;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
            
            ESP_LOGI(TAG, "Playback state changed to: %s", get_playback_state_name(current_playback_state));
        }
    }

    static const char* get_passthrough_cmd_name(uint8_t cmd) {
        switch (cmd) {
            case ESP_AVRC_PT_CMD_PLAY: return "PLAY";
            case ESP_AVRC_PT_CMD_PAUSE: return "PAUSE";
            case ESP_AVRC_PT_CMD_STOP: return "STOP";
            case ESP_AVRC_PT_CMD_FORWARD: return "FORWARD";
            case ESP_AVRC_PT_CMD_BACKWARD: return "BACKWARD";
            case ESP_AVRC_PT_CMD_FAST_FORWARD: return "FAST_FORWARD";
            case ESP_AVRC_PT_CMD_REWIND: return "REWIND";
            default: return "UNKNOWN";
        }
    }

    static const char* get_playback_state_name(esp_avrc_playback_stat_t state) {
        switch (state) {
            case ESP_AVRC_PLAYBACK_STOPPED: return "STOPPED";
            case ESP_AVRC_PLAYBACK_PLAYING: return "PLAYING";
            case ESP_AVRC_PLAYBACK_PAUSED: return "PAUSED";
            case ESP_AVRC_PLAYBACK_FWD_SEEK: return "FORWARD_SEEK";
            case ESP_AVRC_PLAYBACK_REV_SEEK: return "REVERSE_SEEK";
            case ESP_AVRC_PLAYBACK_ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }
};

// Static member definitions
esp_avrc_playback_stat_t BluetoothAudioApp::current_playback_state = ESP_AVRC_PLAYBACK_STOPPED;
BluetoothAudioApp* BluetoothAudioApp::instance = nullptr;

} // namespace btspeaker

// C interface functions for global access
extern "C" {

void app_bluetooth_audio_init() {
    static btspeaker::BluetoothAudioApp app;
    app.init();
}

void app_bluetooth_audio_run() {
    static btspeaker::BluetoothAudioApp app;
    app.run();
}

}

