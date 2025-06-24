#pragma once

#include "esp_raii_wrapper.hpp"
#include "audio_types.hpp"
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
        LLMPipeline(const LLMPipeline &) = delete;

        LLMPipeline &operator=(const LLMPipeline &) = delete;

        void shutdown();

        // Queue audio chunk for streaming playback
        esp_err_t queueAudioChunk(const uint8_t *audio_data, size_t length);

        // Control functions
        esp_err_t stop();

        esp_err_t pause();

        esp_err_t resume();

        esp_err_t setupPipeline();

        // Get current state
        [[nodiscard]] State getState() const { return state_.load(); }

    private:
        static constexpr const char *TAG = "LLM_PIPELINE";
        static constexpr int DEFAULT_SAMPLE_RATE = 24000; // Gemini typical output
        static constexpr int DEFAULT_CHANNELS = 1;
        static constexpr int DEFAULT_BITS = 16;

        // Audio playback task
        static void audioPlaybackTask(void *arg);

        void runAudioPlayback();

        // Pipeline management

        // Audio processing
        esp_err_t writeAudioToPipeline(const uint8_t *data, size_t length);

        // State management
        void setState(State new_state);

        // Member variables
        std::atomic<State> state_;
        TaskHandle_t playbackTaskHandle_;
        QueueHandle_t audioDataQueue_;

        std::unique_ptr<I2SAudioPipeline> audioPipeline;

        // Audio processing
        std::vector<uint8_t> audioBuffer_;
    };

    struct AudioDataItem {
        std::vector<uint8_t> data;
    };

} // namespace btspeaker 