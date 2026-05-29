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

        [[nodiscard]] size_t getBufferFillLevel() const;

        [[nodiscard]] bool isPlaybackIdle() const;

        void setupPipeline() override;

        void cleanupPipeline() override;

    private:
        static constexpr const char *TAG = "LLM_PIPELINE";
        static constexpr int DEFAULT_SAMPLE_RATE = 24000; // Gemini typical output
        static constexpr int DEFAULT_CHANNELS = 1;
        static constexpr int DEFAULT_BITS = 16;
        static constexpr size_t kRawRingBufferSize = 512 * 1024;
        static constexpr size_t kWriteChunkSize = 8192;
        // ~50 ms of 24 kHz mono 16-bit PCM
        static constexpr size_t kPlaybackIdleThresholdBytes = 2400;

        std::unique_ptr<I2SAudioPipeline> audioPipeline;
    };

} // namespace btspeaker 