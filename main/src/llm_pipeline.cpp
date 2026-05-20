#include "../include/llm_pipeline.hpp"
#include "../include/audio_pipeline_util.hpp"
#include <algorithm>

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "ringbuf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    LLMPipeline::LLMPipeline() : BaseAudioPipeline(TAG) {
        ESP_LOGI(TAG, "Initializing LLM pipeline");
    }


    void LLMPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up LLM response audio pipeline (24kHz mono 16-bit)");

        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = static_cast<int>(kRawRingBufferSize),
        };

        audio_element_handle_t raw_handle = raw_stream_init(&raw_cfg);

        ESP_LOGI(TAG, "Creating I2S pipeline with: %dHz, %dch, %dbit",
                 DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, DEFAULT_BITS);

        audioPipeline = AudioPipelineUtil::createAudioPipeline(
                AudioSource::VOICE_RESPONSE,
                raw_handle,
                false,
                DEFAULT_SAMPLE_RATE,
                DEFAULT_CHANNELS,
                DEFAULT_BITS
        );

        audio_pipeline_run(audioPipeline->pipeline.get());

        running = true;

        ESP_LOGI(TAG, "LLM response audio pipeline setup completed successfully");
    }

    void LLMPipeline::cleanupPipeline() {
        ESP_LOGI(TAG, "Cleaning up LLM response audio pipeline");

        if (isRunning() && audioPipeline) {
            AudioPipelineUtil::cleanupPipeline(*audioPipeline);
            audioPipeline.reset();
        }

        ESP_LOGI(TAG, "LLM response audio pipeline cleanup completed");
    }

    esp_err_t LLMPipeline::writeAudioToPipeline(const uint8_t *data, size_t length) {
        if (!audioPipeline || !audioPipeline->pipeline || !data || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        auto *raw = audioPipeline->i2SStreamReader.get();
        if (!raw) {
            return ESP_ERR_INVALID_STATE;
        }

        size_t offset = 0;
        uint32_t backoff_ms = 1;

        while (offset < length) {
            size_t to_write = std::min(kWriteChunkSize, length - offset);

            ringbuf_handle_t rb = audio_element_get_output_ringbuf(raw);
            const size_t free_bytes = rb ? rb_bytes_available(rb) : 0;
            if (free_bytes == 0) {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                if (backoff_ms < 10) {
                    backoff_ms *= 2;
                }
                continue;
            }

            if (free_bytes < to_write) {
                to_write = free_bytes;
            }
            backoff_ms = 1;

            const int written = raw_stream_write(raw, (char *) (data + offset), to_write);
            if (written <= 0) {
                ESP_LOGW(TAG, "raw_stream_write stalled at %u/%u bytes",
                         static_cast<unsigned>(offset),
                         static_cast<unsigned>(length));
                return offset > 0 ? ESP_OK : ESP_FAIL;
            }

            offset += static_cast<size_t>(written);
        }

        return ESP_OK;
    }

    size_t LLMPipeline::getBufferFillLevel() const {
        if (!running || !audioPipeline) {
            return 0;
        }

        size_t filled = 0;
        if (audioPipeline->i2SStreamReader) {
            ringbuf_handle_t rb = audio_element_get_output_ringbuf(audioPipeline->i2SStreamReader.get());
            if (rb) {
                filled += rb_bytes_filled(rb);
            }
        }
        if (audioPipeline->i2SStreamWriter) {
            ringbuf_handle_t rb = audio_element_get_input_ringbuf(audioPipeline->i2SStreamWriter.get());
            if (rb) {
                filled += rb_bytes_filled(rb);
            }
        }
        return filled;
    }

    bool LLMPipeline::isPlaybackIdle() const {
        return getBufferFillLevel() < kPlaybackIdleThresholdBytes;
    }

} // namespace btspeaker 