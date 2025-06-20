#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include "memory_utils.hpp"

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
}

namespace btspeaker {

    using WakeWordCallback = std::function<void()>;

/**
 * @brief Manages wake word detection using ESP-SR
 * 
 * Responsible for:
 * - ESP-SR initialization and configuration
 * - Wake word model loading
 * - Audio processing for wake word detection via dedicated task
 * - Callback notification when wake word is detected
 * - Ring buffer management for continuous audio processing
 */
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
        esp_err_t feedAudioData(const uint8_t* stereo_data, size_t data_length);

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
        static constexpr const char* TAG = "WakeWordDetector";
        
        // Task configuration
        static constexpr size_t WAKE_WORD_TASK_STACK_SIZE = 4096;
        static constexpr UBaseType_t WAKE_WORD_TASK_PRIORITY = 5;
        static constexpr size_t RING_BUFFER_SIZE = 8192;  // 8KB ring buffer for audio data
        
        // ESP-SR state
        std::atomic<bool> enabled_;

        // Task management
        TaskHandle_t processingTaskHandle;
        RingbufHandle_t audioRingBuffer;
        std::unique_ptr<PSRAMRingBuffer> psramRingBuffer_;
        SemaphoreHandle_t enableMutex;
        std::atomic<bool> taskRunning;
        
        // ESP-SR AFE components
        void* afeHandle;        // esp_afe_sr_iface_t*
        void* afeData;          // esp_afe_sr_data_t*
        int feedChunkSize;
        int fetchChunkSize;
        
        // Callback
        WakeWordCallback wakeWordCallback;
        
        // Internal methods
        esp_err_t setupEspSr();
        void cleanupEspSr();
        
        esp_err_t startProcessingTask();
        void stopProcessingTask();
        static void wakeWordTask(void* arg);
        void runWakeWordProcessing();
        
        // Audio processing methods - updated for dual microphone processing
        bool processStereoAudioChunk(const int16_t* stereoSamples, size_t frameCount);
    };

} // namespace btspeaker 