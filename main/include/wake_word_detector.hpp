#pragma once

#include <memory>
#include <functional>
#include <atomic>

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
         * This will convert stereo to mono and feed to ring buffer for processing
         * @param stereo_data Raw stereo audio data (uint8_t format)
         * @param data_length Length of data in bytes
         * @return ESP_OK on success
         */
        esp_err_t feed_audio_data(const uint8_t* stereo_data, size_t data_length);

        /**
         * @brief Set callback for wake word detection
         */
        void set_wake_word_callback(WakeWordCallback callback);

        /**
         * @brief Enable/disable wake word detection
         */
        void set_enabled(bool enabled);

        /**
         * @brief Check if wake word detection is enabled
         */
        bool is_enabled() const { return enabled_; }

    private:
        static constexpr const char* TAG = "WakeWordDetector";
        
        // Task configuration
        static constexpr size_t WAKE_WORD_TASK_STACK_SIZE = 4096;
        static constexpr UBaseType_t WAKE_WORD_TASK_PRIORITY = 5;
        static constexpr size_t RING_BUFFER_SIZE = 8192;  // 8KB ring buffer for audio data
        
        // ESP-SR state
        std::atomic<bool> enabled_;
        float threshold_;
        
        // Task management
        TaskHandle_t processing_task_handle_;
        RingbufHandle_t audio_ring_buffer_;
        SemaphoreHandle_t enable_mutex_;
        std::atomic<bool> task_running_;
        
        // ESP-SR AFE components
        void* afe_handle_;        // esp_afe_sr_iface_t*
        void* afe_data_;          // esp_afe_sr_data_t*
        void* wake_word_models_;  // srmodel_list_t*
        int feed_chunksize_;
        int fetch_chunksize_;
        
        // Audio processing - removed mono conversion buffer
        // ESP-SR AFE now processes dual microphone data directly
        
        // Callback
        WakeWordCallback wake_word_callback_;
        
        // Internal methods
        esp_err_t setup_esp_sr();
        void cleanup_esp_sr();
        
        esp_err_t start_processing_task();
        void stop_processing_task();
        static void wake_word_task(void* arg);
        void run_wake_word_processing();
        
        // Audio processing methods - updated for dual microphone processing
        bool process_stereo_audio_chunk(const int16_t* stereo_samples, size_t frame_count);
    };

} // namespace btspeaker 