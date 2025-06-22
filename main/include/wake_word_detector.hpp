#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include "memory_utils.hpp"
#include "micro_model_config.hpp"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
}

namespace btspeaker {

    using WakeWordCallback = std::function<void()>;

    class WakeWordDetector {
    public:
        WakeWordDetector();

        ~WakeWordDetector();

        // Delete copy operations
        WakeWordDetector(const WakeWordDetector &) = delete;

        WakeWordDetector &operator=(const WakeWordDetector &) = delete;

        /**
         * @brief Initialize wake word detection
         */
        esp_err_t init();

        /**
         * @brief Shutdown wake word detector
         */
        void shutdown();

        /**
         * @brief Feed stereo audio data to wake word detector (thread-safe)
         * @param stereo_data Raw stereo audio data (uint8_t format)
         * @param data_length Length of data in bytes
         * @return ESP_OK on success
         */
        esp_err_t feedAudioData(const uint8_t *stereo_data, size_t data_length);

        /**
         * @brief Set callback for wake word detection
         */
        void setWakeWordCallback(WakeWordCallback callback);

        /**
         * @brief Enable/disable wake word detection
         */
        void setEnabled(bool enabled);

        /**
         * @brief Check if wake word detection is enabled
         */
        [[nodiscard]] bool isEnabled() const { return enabled_; }

    private:
        static constexpr const char *TAG = "WAKE_WORD_DETECTOR";

        esp_err_t startProcessingTask();

        void stopProcessingTask();

        void processWindow(const int16_t *audio_window);

        tflite::MicroMutableOpResolver<6> micro_op_resolver;
        tflite::MicroErrorReporter error_reporter_;
        tflite::MicroInterpreter *interpreter_;
        TfLiteTensor *input_tensor_;
        TfLiteTensor *output_tensor_;

        static tflite::MicroErrorReporter micro_error_reporter;
        static constexpr int kTensorArenaSize = 100 * 1024;
        static uint8_t tensor_arena[kTensorArenaSize];

        static QueueHandle_t audio_queue;

        // Detection logic
        float detection_threshold_ = 0.30f;
        int detection_hits_required_ = 1;
        int detection_hit_count_ = 0;

        // Threading
        std::atomic<bool> enabled_;
        TaskHandle_t processingTaskHandle;

        // Audio processing
        std::unique_ptr<PSRAMRingBuffer> psramRingBuffer_;
        RingbufHandle_t audioRingBuffer;

        // Callback
        WakeWordCallback wakeWordCallback;
    };

} // namespace btspeaker 