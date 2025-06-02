#pragma once

#include "esp_handle.hpp"
#include <memory>
#include <functional>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "raw_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

// Callback type for audio data
using AudioDataCallback = std::function<void(const uint8_t* data, size_t length)>;

/**
 * @brief Manages low-level microphone input pipeline
 * 
 * Responsible for:
 * - I2S microphone stream configuration
 * - Audio pipeline setup and management
 * - Raw audio data streaming to callback
 */
class MicrophoneManager {
public:
    MicrophoneManager();
    ~MicrophoneManager();
    
    // Delete copy operations
    MicrophoneManager(const MicrophoneManager&) = delete;
    MicrophoneManager& operator=(const MicrophoneManager&) = delete;
    
    /**
     * @brief Initialize microphone pipeline
     */
    esp_err_t init();
    
    /**
     * @brief Start microphone recording
     */
    esp_err_t start_recording();
    
    /**
     * @brief Stop microphone recording
     */
    esp_err_t stop_recording();
    
    /**
     * @brief Shutdown microphone manager
     */
    void shutdown();
    
    /**
     * @brief Set callback for audio data
     */
    void set_audio_callback(AudioDataCallback callback);
    
    /**
     * @brief Check if microphone is currently recording
     */
    bool is_recording() const { return recording_; }

private:
    static constexpr const char* TAG = "MIC_MGR";
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int AUDIO_BUFFER_SIZE = 1024;
    
    // Audio processing task
    static void audio_task(void* arg);
    void run_audio_processing();
    
    // Pipeline setup
    esp_err_t setup_microphone_pipeline();
    esp_err_t cleanup_pipeline();
    
    // Member variables
    bool recording_;
    TaskHandle_t audio_task_handle_;
    AudioDataCallback audio_callback_;
    
    // Pipeline components
    PipelineHandle mic_pipeline_;
    ElementHandle i2s_mic_stream_;
    ElementHandle raw_mic_stream_;
    EventIfaceHandle mic_evt_;
};

} // namespace btspeaker 