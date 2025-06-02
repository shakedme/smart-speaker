#pragma once

#include "esp_handle.hpp"
#include "audio_pipeline_manager.hpp"
#include <memory>
#include <atomic>
#include <vector>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "raw_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
}

namespace btspeaker {

class AudioResponsePlayer {
public:
    enum class State {
        IDLE,
        PREPARING,
        PLAYING,
        PAUSED,
        ERROR
    };

    AudioResponsePlayer();
    ~AudioResponsePlayer();
    
    // Delete copy operations
    AudioResponsePlayer(const AudioResponsePlayer&) = delete;
    AudioResponsePlayer& operator=(const AudioResponsePlayer&) = delete;
    
    esp_err_t init(AudioPipelineManager* main_pipeline_manager);
    void shutdown();
    
    // Play audio response (temporarily pause Bluetooth if needed)
    esp_err_t play_audio_response(const uint8_t* audio_data, size_t length);
    
    // Queue audio chunk for streaming playback
    esp_err_t queue_audio_chunk(const uint8_t* audio_data, size_t length, bool is_final);
    
    // Control functions
    esp_err_t stop_playback();
    esp_err_t pause_playback();
    esp_err_t resume_playback();
    
    State get_state() const { return state_.load(); }
    bool is_playing() const { return state_.load() == State::PLAYING; }

private:
    static constexpr const char* TAG = "AUDIO_RESPONSE";
    static constexpr size_t AUDIO_CHUNK_SIZE = 1024;
    static constexpr int RESPONSE_SAMPLE_RATE = 24000; // Gemini typical output
    static constexpr int RESPONSE_CHANNELS = 1;
    static constexpr int RESPONSE_BITS = 16;
    
    // Audio playback task
    static void audio_playback_task(void* arg);
    void run_audio_playback();
    
    // Pipeline management
    esp_err_t setup_response_pipeline();
    esp_err_t cleanup_response_pipeline();
    esp_err_t temporarily_pause_main_pipeline();
    esp_err_t restore_main_pipeline();
    
    // Audio processing
    esp_err_t write_audio_to_pipeline(const uint8_t* data, size_t length);
    esp_err_t convert_audio_format(const uint8_t* input, size_t input_len, 
                                  std::vector<uint8_t>& output);
    
    // State management
    void set_state(State new_state);
    
    // Member variables
    std::atomic<State> state_;
    TaskHandle_t playback_task_handle_;
    QueueHandle_t audio_data_queue_;
    
    // Pipeline components
    AudioPipelineManager* main_pipeline_manager_;
    PipelineHandle response_pipeline_;
    ElementHandle raw_response_stream_;
    ElementHandle i2s_response_writer_;
    EventIfaceHandle response_evt_;
    
    // Playback state
    bool main_pipeline_was_playing_;
    std::vector<uint8_t> audio_buffer_;
    size_t buffer_position_;
    
    // Configuration
    bool auto_resume_main_pipeline_;
};

// Audio data queue item
struct AudioDataItem {
    std::vector<uint8_t> data;
    bool is_final;
};

} // namespace btspeaker 