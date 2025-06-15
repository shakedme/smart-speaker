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
    
    // Queue audio chunk for streaming playback
    esp_err_t queueAudioChunk(const uint8_t* audio_data, size_t length, bool is_final);
    
    // Control functions
    esp_err_t stopPlayback();
    esp_err_t pausePlayback();
    esp_err_t resumePlayback();
    
    // State query
    [[nodiscard]] bool is_playing() const { return state_.load() == State::PLAYING; }

private:
    static constexpr const char* TAG = "LLM_PIPELINE";
    static constexpr size_t AUDIO_CHUNK_SIZE = 1024;
    static constexpr int DEFAULT_SAMPLE_RATE = 24000; // Gemini typical output
    static constexpr int DEFAULT_CHANNELS = 1;
    static constexpr int DEFAULT_BITS = 16;
    
    // Audio playback task
    static void audioPlaybackTask(void* arg);
    void runAudioPlayback();
    
    // Pipeline management
    esp_err_t setupResponsePipeline();
    esp_err_t cleanupResponsePipeline();
    
    // Audio processing
    esp_err_t writeAudioToPipeline(const uint8_t* data, size_t length);
    esp_err_t convertAudioFormat(const uint8_t* input, size_t input_len,
                                 std::vector<uint8_t>& output);
    
    // State management
    void set_state(State new_state);
    
    // Member variables
    std::atomic<State> state_;
    TaskHandle_t playbackTaskHandle_;
    QueueHandle_t audioDataQueue_;

    // Pipeline components (independent I2S setup)
    PipelineHandle responsePipeline_;
    ElementHandle rawResponseStream_;
    ElementHandle i2sResponseWriter_;
    EventIfaceHandle responseEvent_;
    
    // Audio processing
    std::vector<uint8_t> audioBuffer_;
};

// Audio data queue item
struct AudioDataItem {
    std::vector<uint8_t> data;
    bool isFinal;
};

} // namespace btspeaker 