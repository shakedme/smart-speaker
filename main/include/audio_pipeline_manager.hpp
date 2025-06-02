#pragma once

#include "esp_handle.hpp"
#include <cstring>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"  // Use centralized pin definitions
}

namespace btspeaker {

// - I2S_BCK_IO (25): Bit Clock → PCM5102A BCK
// - I2S_WS_IO (22):  Word Select → PCM5102A LCK  
// - I2S_DO_IO (26):  Data Out → PCM5102A DIN

class AudioPipelineManager {
public:
    AudioPipelineManager() = default;
    ~AudioPipelineManager() = default;
    
    // Delete copy operations
    AudioPipelineManager(const AudioPipelineManager&) = delete;
    AudioPipelineManager& operator=(const AudioPipelineManager&) = delete;
    
    // Move operations
    AudioPipelineManager(AudioPipelineManager&&) = default;
    AudioPipelineManager& operator=(AudioPipelineManager&&) = default;
    
    esp_err_t setup_pipeline();
    esp_err_t start_pipeline();
    esp_err_t stop_pipeline();
    esp_err_t pause_pipeline();
    esp_err_t resume_pipeline();
    
    esp_err_t run_event_loop();
    
    bool is_pipeline_ready() const { return pipeline_ && i2s_stream_writer_ && bt_stream_reader_; }
    
    void cleanup();

private:
    bool handle_audio_event(const audio_event_iface_msg_t& msg);
    
    // RAII-managed resources
    PipelineHandle pipeline_;
    ElementHandle i2s_stream_writer_;
    ElementHandle bt_stream_reader_;
    EventIfaceHandle evt_;
    bool bt_service_started_ = false;
};

} // namespace btspeaker 