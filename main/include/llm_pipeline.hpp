#pragma once

#include "esp_raii_wrapper.hpp"
#include "audio_types.hpp"
#include "base_audio_pipeline.hpp"
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

    class BaseAudioPipeline;

    class LLMPipeline : public BaseAudioPipeline {
    public:

        LLMPipeline();

        ~LLMPipeline() = default;

        esp_err_t writeAudioToPipeline(const uint8_t *data, size_t length);

        void setupPipeline() override;

        void cleanupPipeline() override;

    private:
        static constexpr const char *TAG = "LLM_PIPELINE";
        static constexpr int DEFAULT_SAMPLE_RATE = 24000; // Gemini typical output
        static constexpr int DEFAULT_CHANNELS = 1;
        static constexpr int DEFAULT_BITS = 16;

        std::unique_ptr<I2SAudioPipeline> audioPipeline;
    };

} // namespace btspeaker 