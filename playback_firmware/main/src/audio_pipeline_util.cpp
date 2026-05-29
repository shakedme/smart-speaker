#include "../include/audio_pipeline_util.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include <bluetooth_service.h>
#include "esp_err.h"
#include "board.h"
}

namespace btspeaker {

    std::unique_ptr<I2SAudioPipeline> AudioPipelineUtil::createAudioPipeline(AudioSource source,
                                                                             audio_element_handle_t reader,
                                                                             bool isInput,
                                                                             int sampleRate,
                                                                             int channels,
                                                                             int bitsPerSample) {
        ESP_LOGI(TAG, "Creating audio pipeline for source %d (%dHz, %dch, %dbit)",
                 static_cast<int>(source), sampleRate, channels, bitsPerSample);

        auto audioPipeline = std::make_unique<I2SAudioPipeline>();

        // Create I2S writer
        auto i2SHandle = createI2SWriter(
                source,
                sampleRate,
                channels,
                bitsPerSample
        );
        if (!i2SHandle) {
            ESP_LOGE(TAG, "Failed to create I2S stream via shared service");
            throw std::runtime_error("Failed to create I2S stream");
        }
        audioPipeline->i2SStreamWriter.reset(i2SHandle);
        audioPipeline->i2SStreamReader.reset(reader);

        // Create the audio pipeline
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline_handle) {
            ESP_LOGE(TAG, "Failed to create audio pipeline");
            throw std::runtime_error("Failed to create audio pipeline");
        }
        audioPipeline->pipeline.reset(pipeline_handle, audioPipelineDeleter);


        registerAndLinkPipeline(*audioPipeline);

        ESP_LOGI(TAG, "Audio pipeline setup completed successfully");

        return audioPipeline;
    }

    void AudioPipelineUtil::registerAndLinkPipeline(I2SAudioPipeline &audioPipeline) {
        // Register and link elements
        esp_err_t ret = audio_pipeline_register(audioPipeline.pipeline.get(),
                                                audioPipeline.i2SStreamReader.get(),
                                                "src");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register source stream: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to register stream");
        }

        ret = audio_pipeline_register(audioPipeline.pipeline.get(),
                                      audioPipeline.i2SStreamWriter.get(),
                                      "i2s");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register I2S stream: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to register I2S stream");
        }

        const char *link_tag[] = {"src", "i2s"};
        ret = audio_pipeline_link(audioPipeline.pipeline.get(), link_tag, 2);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link pipeline elements: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to link pipeline elements");
        }

        // No listener is attached here. This path does not consume ADF events, and
        // an undrained listener queue fills quickly during pause/resume churn.
    }

    audio_element_handle_t AudioPipelineUtil::createI2SWriter(AudioSource source,
                                                              int sample_rate,
                                                              int channels,
                                                              int bits_per_sample) {

        ESP_LOGI(TAG, "Creating I2S writer for source %d (%dHz, %dch, %dbit)",
                 static_cast<int>(source), sample_rate, channels, bits_per_sample);

        // Create I2S stream configuration
        i2s_stream_cfg_t i2SConfig = createI2SConfig(source, sample_rate, channels, bits_per_sample);

        // Create the I2S stream element
        audio_element_handle_t i2SHandle = i2s_stream_init(&i2SConfig);
        if (!i2SHandle) {
            ESP_LOGE(TAG, "Failed to create I2S stream for source %d", static_cast<int>(source));
            return nullptr;
        }

        ESP_LOGI(TAG, "Created I2S writer for source %d", static_cast<int>(source));
        return i2SHandle;
    }

    i2s_stream_cfg_t AudioPipelineUtil::createI2SConfig(AudioSource source,
                                                        int sampleRate,
                                                        int channels,
                                                        int bitsPerSample) {
        int taskPriority;
        uint32_t outRingBufferSize;
        int i2sBufferLen;
        uint32_t dmaDescNum;
        uint32_t dmaFrameNum;

        switch (source) {
            case AudioSource::BLUETOOTH:
                taskPriority = 25;
                outRingBufferSize = 64 * 1024;
                i2sBufferLen = 12000;
                dmaDescNum = 16;
                dmaFrameNum = 512;
                break;
            case AudioSource::SPOTIFY:
                taskPriority = 26;
                outRingBufferSize = 256 * 1024;
                // IDF5 I2S stream requires buffer_len % 12 == 0 (8192 is invalid)
                i2sBufferLen = 8196;
                dmaDescNum = 16;
                dmaFrameNum = 1023;
                break;
            case AudioSource::VOICE_RESPONSE:
                taskPriority = 27;
                outRingBufferSize = 64 * 1024;
                i2sBufferLen = 12000;
                dmaDescNum = 16;
                dmaFrameNum = 512;
                break;
            default:
                ESP_LOGE(TAG, "Unknown audio source %d", static_cast<int>(source));
                return {};
        }

        // Create standardized I2S configuration using board.h pins
        i2s_stream_cfg_t i2SCfg = {
                .type = AUDIO_STREAM_WRITER,
                .transmit_mode = I2S_COMM_MODE_STD,
                .chan_cfg = {
                        .id = static_cast<i2s_port_t>(I2S_PORT),
                        .role = I2S_ROLE_MASTER,
                        .dma_desc_num = dmaDescNum,
                        .dma_frame_num = dmaFrameNum,
                        .auto_clear = true,
                        .auto_clear_before_cb = false,
                        .allow_pd = false,
                        .intr_priority = 0
                },
                .std_cfg = {
                        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sampleRate)),
                        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                static_cast<i2s_data_bit_width_t>(bitsPerSample),
                                static_cast<i2s_slot_mode_t>(channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO)
                        ),
                        .gpio_cfg = {
                                .mclk = GPIO_NUM_NC,
                                .bclk = static_cast<gpio_num_t>(I2S_BCK_IO), // GPIO25 from board.h
                                .ws   = static_cast<gpio_num_t>(I2S_WS_IO),  // GPIO22 from board.h
                                .dout = static_cast<gpio_num_t>(I2S_DO_IO),  // GPIO26 from board.h
                                .din  = GPIO_NUM_NC,
                                .invert_flags = {
                                        .mclk_inv = false,
                                        .bclk_inv = false,
                                        .ws_inv = false
                                }
                        },
                },
                .pdm_rx_cfg = {},
                .use_alc = false,
                .volume = DEFAULT_VOLUME,
                .out_rb_size = static_cast<int>(outRingBufferSize),
                .task_stack = 4096,
                .task_core = 0,
                .task_prio = taskPriority,
                .stack_in_ext = true,
                .multi_out_num = 0,
                .uninstall_drv = true,
                .need_expand = false,
                .expand_src_bits = static_cast<i2s_data_bit_width_t>(bitsPerSample),
                .buffer_len = i2sBufferLen
        };

        ESP_LOGD(TAG, "Created I2S config for source %d: %dHz, %dch, %dbit, prio=%d",
                 static_cast<int>(source), sampleRate, channels, bitsPerSample, taskPriority);

        return i2SCfg;
    }

    void AudioPipelineUtil::stopPipeline(I2SAudioPipeline &pipeline) {
        if (pipeline.pipeline) {
            esp_err_t err = audio_pipeline_stop(pipeline.pipeline.get());
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to stop pipeline: %s", esp_err_to_name(err));
            }
            err = audio_pipeline_wait_for_stop(pipeline.pipeline.get());
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to wait for pipeline stop: %s", esp_err_to_name(err));
            }
        }

        ESP_LOGI(TAG, "Pipeline stop completed");
    }

    void AudioPipelineUtil::cleanupPipeline(I2SAudioPipeline &pipeline) {
        if (pipeline.pipeline) pipeline.pipeline.reset();
        if (pipeline.i2SStreamReader) pipeline.i2SStreamReader.reset();
        if (pipeline.i2SStreamWriter) pipeline.i2SStreamWriter.reset();
        if (pipeline.evtHandle) pipeline.evtHandle.reset();
        ESP_LOGI(TAG, "Pipeline cleanup completed");
    }

} // namespace btspeaker 