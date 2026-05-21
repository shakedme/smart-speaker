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
        static constexpr int AUDIO_BUFFER_SIZE = 1024 * 4;
        static constexpr float SAMPLE_RATE = 16000.0f;
        static constexpr float HPF_CUTOFF_HZ = 150.0f;        // speech band: reject bass bleed from playback
        static constexpr float LPF_CUTOFF_HZ = 5000.0f;       // speech band: reject treble/hi-hat bleed
        static constexpr float HPF_F_NORM = HPF_CUTOFF_HZ / SAMPLE_RATE;
        static constexpr float LPF_F_NORM = LPF_CUTOFF_HZ / SAMPLE_RATE;
        static constexpr float Q_FACTOR = 0.707f;             // Butterworth-style two-pole (~1/√2)
        static constexpr int SAMPLE_COUNT = AUDIO_BUFFER_SIZE / sizeof(int32_t);
        static constexpr int MIC_DIAG_INTERVAL_MS = 5000;

        void logMicDiagnostics(int64_t now_us,
                               uint32_t loop_count,
                               uint32_t callback_count,
                               uint64_t total_bytes_read,
                               uint64_t total_bytes_out,
                               uint32_t i2s_errors,
                               uint32_t zero_reads,
                               uint64_t sum_i2s_read_us,
                               uint32_t max_i2s_read_us,
                               uint64_t sum_process_us,
                               uint32_t max_process_us,
                               uint64_t sum_callback_us,
                               uint32_t max_callback_us) const;

        float hpf_delay_lines[2] = {0.0f, 0.0f};
        float lpf_delay_lines[2] = {0.0f, 0.0f};
        float hpf_coeffs[5] = {};               // b0,b1,b2,a1,a2
        float lpf_coeffs[5] = {};

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