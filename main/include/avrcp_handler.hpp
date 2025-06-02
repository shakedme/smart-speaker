#pragma once

#include <atomic>

extern "C" {
#include "esp_err.h"
#include "esp_avrc_api.h"
}

namespace btspeaker {

// Forward declaration
class AudioPipelineManager;

class AvrcpHandler {
public:
    AvrcpHandler();
    ~AvrcpHandler();
    
    // Delete copy operations
    AvrcpHandler(const AvrcpHandler&) = delete;
    AvrcpHandler& operator=(const AvrcpHandler&) = delete;
    
    // Move operations are also deleted since we have static instance management
    AvrcpHandler(AvrcpHandler&&) = delete;
    AvrcpHandler& operator=(AvrcpHandler&&) = delete;
    
    esp_err_t configure_target();
    void set_pipeline_manager(AudioPipelineManager* manager);
    
    // Static callback for ESP-IDF C API
    static void avrcp_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
    
    // Instance-based event handling
    void handle_avrcp_event(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

private:
    void handle_avrcp_passthrough(uint8_t key_code, uint8_t key_state);
    void handle_avrcp_notification_registration(uint8_t event_id);
    void set_playback_state(esp_avrc_playback_stat_t new_state);
    
    static const char* get_passthrough_cmd_name(uint8_t cmd);
    static const char* get_playback_state_name(esp_avrc_playback_stat_t state);
    
    // Simple static instance for C callback access
    static AvrcpHandler* instance_;
    
    std::atomic<esp_avrc_playback_stat_t> current_playback_state_;
    AudioPipelineManager* pipeline_manager_;
};

} // namespace btspeaker 