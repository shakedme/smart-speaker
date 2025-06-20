#include "../include/audio_pipeline_util.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_check.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include <bluetooth_service.h>
#include "audio_event_iface.h"
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

        // Create pipeline event handle
        audio_event_iface_cfg_t evtCfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        audio_event_iface_handle_t evtHandle = audio_event_iface_init(&evtCfg);
        if (!evtHandle) {
            ESP_LOGE(TAG, "Failed to create audio event interface");
            throw std::runtime_error("Failed to create audio event interface");
        }
        audioPipeline->evtHandle.reset(evtHandle, audioEventIfaceDeleter);

        // Create I2S writer
        auto i2SHandle = createI2SWriter(
                AudioSource::BLUETOOTH,
                sampleRate,
                channels,
                bitsPerSample
        );
        if (!i2SHandle) {
            ESP_LOGE(TAG, "Failed to create I2S stream via shared service");
            throw std::runtime_error("Failed to create I2S stream");
        }
        audioPipeline->i2SStreamWriter.reset(i2SHandle, audioElementDeleter);

        // Create I2S reader
        audioPipeline->i2SStreamReader.reset(reader, audioElementDeleter);

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

        // Set event listener on pipeline
        ret = audio_pipeline_set_listener(audioPipeline.pipeline.get(), audioPipeline.evtHandle.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set pipeline listener: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to set pipeline listener");
        }
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

        switch (source) {
            case AudioSource::BLUETOOTH:
                taskPriority = 23; // Lower priority for Bluetooth
                break;
            case AudioSource::SPOTIFY:
                taskPriority = 24; // Lower priority for Spotify
                break;
            case AudioSource::VOICE_RESPONSE:
                taskPriority = 25; // Higher priority for voice
                break;
            default:
                ESP_LOGE(TAG, "Unknown audio source %d", static_cast<int>(source));
                return {}; // Return empty config on error
        }

        // Create standardized I2S configuration using board.h pins
        i2s_stream_cfg_t i2SCfg = {
                .type = AUDIO_STREAM_WRITER,
                .transmit_mode = I2S_COMM_MODE_STD,
                .chan_cfg = {
                        .id = static_cast<i2s_port_t>(I2S_PORT),
                        .role = I2S_ROLE_MASTER,
                        .dma_desc_num = 4,
                        .dma_frame_num = 160,
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
                .out_rb_size = 8 * 1024,
                .task_stack = 2048,
                .task_core = 0,
                .task_prio = taskPriority,
                .stack_in_ext = true,
                .multi_out_num = 0,
                .uninstall_drv = false,
                .need_expand = false,
                .expand_src_bits = static_cast<i2s_data_bit_width_t>(bitsPerSample),
                .buffer_len = 2400
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
        if (pipeline.pipeline) {
            if (pipeline.i2SStreamReader) {
                audio_pipeline_unregister(pipeline.pipeline.get(), pipeline.i2SStreamReader.get());
            }
            if (pipeline.i2SStreamWriter) {
                audio_pipeline_unregister(pipeline.pipeline.get(), pipeline.i2SStreamWriter.get());
            }
            audio_pipeline_terminate(pipeline.pipeline.get());
            audio_pipeline_deinit(pipeline.pipeline.get());
        }

        pipeline.i2SStreamWriter.reset();
        pipeline.i2SStreamReader.reset();
        pipeline.evtHandle.reset();
        pipeline.pipeline.reset();

        ESP_LOGI(TAG, "Pipeline cleanup completed");
    }

} // namespace btspeaker 