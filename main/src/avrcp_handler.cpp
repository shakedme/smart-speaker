#include "../include/avrcp_handler.hpp"
#include "../include/audio_pipeline_manager.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

static constexpr const char* TAG = "AVRCP";

// Static member definition - simple and clean
AvrcpHandler* AvrcpHandler::instance_ = nullptr;

AvrcpHandler::AvrcpHandler() 
    : current_playback_state_(ESP_AVRC_PLAYBACK_STOPPED), pipeline_manager_(nullptr) {
    // Simple assignment - only one instance expected in this application
    instance_ = this;
    ESP_LOGD(TAG, "AvrcpHandler created and set as active instance");
}

AvrcpHandler::~AvrcpHandler() {
    // Clean up static instance pointer
    if (instance_ == this) {
        instance_ = nullptr;
        ESP_LOGD(TAG, "AvrcpHandler destroyed and instance cleared");
    }
}

esp_err_t AvrcpHandler::configure_target() {
    ESP_LOGI(TAG, "Configuring AVRCP target");
    
    // Configure supported passthrough commands
    esp_avrc_psth_bit_mask_t cmd_set;
    memset(&cmd_set, 0, sizeof(cmd_set));
    
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FAST_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_REWIND);
    
    esp_err_t ret = esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AVRCP passthrough filter: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure notification capabilities
    esp_avrc_rn_evt_cap_mask_t evt_set;
    memset(&evt_set, 0, sizeof(evt_set));
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
    
    ret = esp_avrc_tg_set_rn_evt_cap(&evt_set);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AVRCP notification capabilities: %s", esp_err_to_name(ret));
        return ret;
    }
    
    current_playback_state_.store(ESP_AVRC_PLAYBACK_STOPPED);
    ESP_LOGI(TAG, "AVRCP Target configured successfully");
    return ESP_OK;
}

void AvrcpHandler::set_pipeline_manager(AudioPipelineManager* manager) {
    pipeline_manager_ = manager;
}

void AvrcpHandler::avrcp_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    if (!instance_) {
        ESP_LOGW(TAG, "AVRCP callback called but no active instance");
        return;
    }
    
    instance_->handle_avrcp_event(event, param);
}

void AvrcpHandler::handle_avrcp_event(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    if (!param) return;
    
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "AVRCP Target connected");
            } else {
                ESP_LOGI(TAG, "AVRCP Target disconnected");
            }
            break;
            
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            handle_avrcp_passthrough(param->psth_cmd.key_code, param->psth_cmd.key_state);
            break;
            
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            ESP_LOGI(TAG, "AVRCP volume command: %d", param->set_abs_vol.volume);
            break;
            
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            handle_avrcp_notification_registration(param->reg_ntf.event_id);
            break;
            
        default:
            ESP_LOGD(TAG, "AVRCP TG event: %d", event);
            break;
    }
}

void AvrcpHandler::handle_avrcp_passthrough(uint8_t key_code, uint8_t key_state) {
    if (key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
        return;
    }
    
    if (!pipeline_manager_ || !pipeline_manager_->is_pipeline_ready()) {
        ESP_LOGW(TAG, "Pipeline not available for AVRCP command");
        return;
    }
    
    ESP_LOGI(TAG, "AVRCP passthrough command: %s", get_passthrough_cmd_name(key_code));
    
    esp_err_t ret = ESP_OK;
    esp_avrc_playback_stat_t new_state = current_playback_state_.load();
    
    switch (key_code) {
        case ESP_AVRC_PT_CMD_PLAY:
            ret = pipeline_manager_->resume_pipeline();
            if (ret == ESP_OK) {
                new_state = ESP_AVRC_PLAYBACK_PLAYING;
            }
            break;
            
        case ESP_AVRC_PT_CMD_PAUSE:
            ret = pipeline_manager_->pause_pipeline();
            if (ret == ESP_OK) {
                new_state = ESP_AVRC_PLAYBACK_PAUSED;
            }
            break;
            
        case ESP_AVRC_PT_CMD_STOP:
            ret = pipeline_manager_->stop_pipeline();
            if (ret == ESP_OK) {
                new_state = ESP_AVRC_PLAYBACK_STOPPED;
            }
            break;
            
        case ESP_AVRC_PT_CMD_FORWARD:
        case ESP_AVRC_PT_CMD_BACKWARD:
            ESP_LOGI(TAG, "Track navigation not implemented for this media source");
            return;
            
        case ESP_AVRC_PT_CMD_FAST_FORWARD:
        case ESP_AVRC_PT_CMD_REWIND:
            ESP_LOGI(TAG, "Seek operations not implemented for this media source");
            return;
            
        default:
            ESP_LOGW(TAG, "Unhandled AVRCP command: 0x%02x", key_code);
            return;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP command failed: %s", esp_err_to_name(ret));
    } else {
        set_playback_state(new_state);
    }
}

void AvrcpHandler::handle_avrcp_notification_registration(uint8_t event_id) {
    ESP_LOGI(TAG, "AVRCP notification registration for event: %d", event_id);
    
    if (event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
        esp_avrc_rn_param_t rn_param;
        rn_param.playback = current_playback_state_.load();
        esp_err_t ret = esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 
                                               ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send AVRCP notification response: %s", esp_err_to_name(ret));
        }
    }
}

void AvrcpHandler::set_playback_state(esp_avrc_playback_stat_t new_state) {
    esp_avrc_playback_stat_t old_state = current_playback_state_.exchange(new_state);
    
    if (old_state != new_state) {
        esp_avrc_rn_param_t rn_param;
        rn_param.playback = new_state;
        esp_err_t ret = esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, 
                                               ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send playback state change notification: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Playback state changed: %s -> %s", 
                    get_playback_state_name(old_state), 
                    get_playback_state_name(new_state));
        }
    }
}

const char* AvrcpHandler::get_passthrough_cmd_name(uint8_t cmd) {
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

const char* AvrcpHandler::get_playback_state_name(esp_avrc_playback_stat_t state) {
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

} // namespace btspeaker 