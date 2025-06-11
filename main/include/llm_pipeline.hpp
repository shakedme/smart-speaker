#pragma once

#include "esp_raii_wrapper.hpp"
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

class LLMPipeline {
public:
    enum class State {
        IDLE,
        PREPARING,
        PLAYING,
        PAUSED,
        ERROR
    };

    LLMPipeline();
    ~LLMPipeline();
    
    // Delete copy operations
    LLMPipeline(const LLMPipeline&) = delete;
    LLMPipeline& operator=(const LLMPipeline&) = delete;
    
    // Initialize the LLM pipeline (no external dependencies)
    esp_err_t init();
    void shutdown();
    
    // Play audio response
    esp_err_t play_audio_response(const uint8_t* audio_data, size_t length);
    
    // Queue audio chunk for streaming playback
    esp_err_t queue_audio_chunk(const uint8_t* audio_data, size_t length, bool is_final);
    
    // Control functions
    esp_err_t stop_playback();
    esp_err_t pause_playback();
    esp_err_t resume_playback();
    
    // State query
    State get_state() const { return state_.load(); }
    bool is_playing() const { return state_.load() == State::PLAYING; }
    
    // Volume control
    void set_volume(uint8_t volume);
    uint8_t get_volume() const { return volume_; }

private:
    static constexpr const char* TAG = "LLM_PIPELINE";
    static constexpr size_t AUDIO_CHUNK_SIZE = 1024;
    static constexpr int DEFAULT_SAMPLE_RATE = 24000; // Gemini typical output
    static constexpr int DEFAULT_CHANNELS = 1;
    static constexpr int DEFAULT_BITS = 16;
    
    // Audio playback task
    static void audio_playback_task(void* arg);
    void run_audio_playback();
    
    // Pipeline management
    esp_err_t setup_response_pipeline();
    esp_err_t cleanup_response_pipeline();
    
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
    uint8_t volume_;
    
    // Pipeline components (independent I2S setup)
    PipelineHandle response_pipeline_;
    ElementHandle raw_response_stream_;
    ElementHandle i2s_response_writer_;
    EventIfaceHandle response_evt_;
    
    // Audio processing
    std::vector<uint8_t> audio_buffer_;
    size_t buffer_position_;
};

// Audio data queue item
struct AudioDataItem {
    std::vector<uint8_t> data;
    bool is_final;
};

} // namespace btspeaker 