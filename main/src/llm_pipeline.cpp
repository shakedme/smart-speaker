#include "../include/llm_pipeline.hpp"
#include "../include/audio_pipeline_util.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "audio_common.h"
#include "i2s_stream.h"
}

namespace btspeaker {

    LLMPipeline::LLMPipeline() : BaseAudioPipeline(TAG) {
        ESP_LOGI(TAG, "Initializing LLM pipeline");
    }


    void LLMPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up LLM response audio pipeline (24kHz mono 16-bit)");

        raw_stream_cfg_t raw_cfg = {
                .type = AUDIO_STREAM_READER,
                .out_rb_size = 16 * 1024,
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
        if (!audioPipeline->pipeline || !data || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        int bytes_written = raw_stream_write(audioPipeline->i2SStreamReader.get(), (char *) data, length);
        if (bytes_written != length) {
            ESP_LOGW(TAG, "Incomplete write: %d/%zu bytes", bytes_written, length);
            return ESP_FAIL;
        }

        return ESP_OK;
    }

} // namespace btspeaker 