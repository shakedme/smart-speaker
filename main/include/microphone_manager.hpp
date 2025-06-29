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

    using AudioDataCallback = std::function<void(const uint8_t *data, size_t length)>;

    class MicrophoneManager {
    public:
        MicrophoneManager();

        ~MicrophoneManager();

        // Delete copy operations
        MicrophoneManager(const MicrophoneManager &) = delete;

        MicrophoneManager &operator=(const MicrophoneManager &) = delete;

        void init();

        esp_err_t startRecording();

        esp_err_t stopRecording();

        void shutdown();

        void setAudioCallback(AudioDataCallback callback);

    private:
        static constexpr const char *TAG = "MIC_MGR";
        static constexpr int AUDIO_BUFFER_SIZE = 1024 * 8;
        static constexpr float CUTOFF_HZ = 50.0f;             // remove DC and sub-50Hz drift
        static constexpr float SAMPLE_RATE = 16000.0f;          // your mic ADC rate
        static constexpr float F_NORM = CUTOFF_HZ / SAMPLE_RATE;  // ≈0.003125
        static constexpr float Q_FACTOR = 0.707f;            // Butterworth-style two-pole (~1/√2)
        static constexpr int SAMPLE_COUNT = AUDIO_BUFFER_SIZE / sizeof(int32_t);

        float delay_lines[2] = {0.0f, 0.0f};
        float coeffs[5];                        // b0,b1,b2,a1,a2

        // Audio processing task
        static void audioTask(void *arg);

        void runAudioProcessing();

        // Pipeline setup
        esp_err_t setupMicrophonePipeline();

        esp_err_t cleanupPipeline();

        void applySPH0645Fix(i2s_port_t i2s_num);  // SPH0645 timing fix function

        // Audio validation and processing
        size_t processMonoAudio(const uint8_t *raw_data, uint8_t *processed_data, size_t raw_bytes);

        // Member variables
        bool recording_;
        TaskHandle_t audioTaskHandle_;
        AudioDataCallback audioCallback_;

        // Memory allocated for task stack and buffer (for cleanup)
        StackType_t *taskStack = nullptr;
        StaticTask_t *taskBuffer = nullptr;

        // Float buffer for audio processing
        float *floatBuffer = nullptr;

        // Direct I2S channel handle (new API)
        i2s_chan_handle_t i2s_rx_handle_ = nullptr;
    };

} // namespace btspeaker 