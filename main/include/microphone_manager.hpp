#pragma once

#include "esp_raii_wrapper.hpp"
#include <memory>
#include <functional>
#include <driver/i2s_types.h>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "raw_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"  // New I2S API
}

namespace btspeaker {

// Callback type for audio data
    using AudioDataCallback = std::function<void(const uint8_t *data, size_t length)>;

/**
 * @brief Manages low-level microphone input pipeline
 * 
 * Responsible for:
 * - I2S microphone stream configuration for SPH0645LM4H MEMS microphone
 * - Audio pipeline setup and management (mono to stereo conversion)
 * - Raw audio data streaming to callback
 * - Single microphone with stereo emulation for compatibility
 */
    class MicrophoneManager {
    public:
        MicrophoneManager();

        ~MicrophoneManager();

        // Delete copy operations
        MicrophoneManager(const MicrophoneManager &) = delete;

        MicrophoneManager &operator=(const MicrophoneManager &) = delete;

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
         * @note Audio data will be stereo (2 channels) - mono input duplicated to both channels
         */
        void setAudioCallback(AudioDataCallback callback);

    private:
        static constexpr const char *TAG = "MIC_MGR";
        static constexpr int AUDIO_BUFFER_SIZE = 512;

        // Audio processing task
        static void audioTask(void *arg);

        void runAudioProcessing();

        // Pipeline setup
        esp_err_t setupMicrophonePipeline();

        esp_err_t cleanupPipeline();

        void applySPH0645Fix(i2s_port_t i2s_num);  // SPH0645 timing fix function

        // Audio validation and processing
        bool hasVoiceActivity(const uint8_t *audio_buffer, size_t bytes_read);  // Lightweight VAD pre-filter
        size_t convertMonoToStereo(const uint8_t *mono_data, uint8_t *stereo_data, size_t mono_bytes);

        // Member variables
        bool recording_;
        TaskHandle_t audioTaskHandle_;
        AudioDataCallback audioCallback_;

        // Direct I2S channel handle (new API)
        i2s_chan_handle_t i2s_rx_handle_ = nullptr;
    };

} // namespace btspeaker 