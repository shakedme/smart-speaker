#pragma once

#include "esp_raii_wrapper.hpp"
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
 * - I2S microphone stream configuration for SPH0645LM4H MEMS microphones
 * - Stereo audio pipeline setup and management  
 * - Raw audio data streaming to callback
 * - Noise reduction capabilities with dual microphone setup
 */
class MicrophoneManager {
public:
    MicrophoneManager();
    ~MicrophoneManager();
    
    // Delete copy operations
    MicrophoneManager(const MicrophoneManager&) = delete;
    MicrophoneManager& operator=(const MicrophoneManager&) = delete;
    
    /**
     * @brief Initialize I2S microphone pipeline for SPH0645LM4H
     */
    esp_err_t init();
    
    /**
     * @brief Start microphone recording
     */
    esp_err_t startRecording();
    
    /**
     * @brief Stop microphone recording
     */
    esp_err_t stopRecording();
    
    /**
     * @brief Shutdown microphone manager
     */
    void shutdown();
    
    /**
     * @brief Set callback for audio data
     * @note Audio data will be stereo (2 channels) from two SPH0645LM4H microphones
     */
    void setAudioCallback(AudioDataCallback callback);

private:
    static constexpr const char* TAG = "MIC_MGR";
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int AUDIO_BUFFER_SIZE = 1024;  // Increased for stereo data
    static constexpr const char* MIC_I2S = "mic_i2s";
    static constexpr const char* MIC_RAW = "mic_raw";

    // Audio processing task
    static void audioTask(void* arg);
    void runAudioProcessing();
    
    // Pipeline setup
    esp_err_t setupMicrophonePipeline();
    esp_err_t cleanupPipeline();
    
    // Member variables
    bool recording_;
    TaskHandle_t audioTaskHandle_;
    AudioDataCallback audioCallback_;
    
    // Pipeline components
    PipelineHandle micPipeline_;
    ElementHandle i2SMicStream_;
    ElementHandle rawMicStream_;
    EventIfaceHandle micEvent_;
};

} // namespace btspeaker 