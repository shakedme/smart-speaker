#include "korvo2_mic_capture_backend.hpp"
#include "korvo2_board_resources.hpp"

extern "C" {
#include "audio_common.h"
#include "audio_hal.h"
#include "audio_pipeline.h"
#include "board_def.h"
#include "es7210.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "filter_resample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_stream.h"
#include "raw_stream.h"
}

namespace btspeaker {

namespace {
constexpr const char *kTag = "KORVO2_MIC";
constexpr int kDestSampleRateHz = 16000;
constexpr int kSrcSampleRateHz = 48000;
constexpr int kMonoSamplesPerFrame = 320;
constexpr int kStereoSamplesPerFrame = kMonoSamplesPerFrame * 2;
constexpr int kCaptureTaskStackWords = 8192;
constexpr UBaseType_t kCaptureTaskPriority = 5;
// Korvo-2 stereo capture with MIC1|MIC3 (non-TDM): talk mic lands on ch0.
constexpr int kMicChannelIndex = 0;

uint32_t computePeakAbs(const int16_t *samples, const size_t count) {
    uint32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t sample = samples[i];
        const uint32_t magnitude = static_cast<uint32_t>(sample < 0 ? -sample : sample);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

void extractMonoFromStereo(const int16_t *stereo,
                           const size_t frameCount,
                           const int channelIndex,
                           int16_t *monoOut) {
    for (size_t i = 0; i < frameCount; ++i) {
        monoOut[i] = stereo[(i * 2U) + static_cast<size_t>(channelIndex)];
    }
}
}  // namespace

Korvo2MicCaptureBackend::~Korvo2MicCaptureBackend() {
    stop();
}

const char *Korvo2MicCaptureBackend::name() const {
    return "korvo2_onboard";
}

esp_err_t Korvo2MicCaptureBackend::init() {
    esp_err_t ret = Korvo2BoardResources::ensureInitialized();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Board init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(kTag, "Korvo-2 onboard mic backend initialized");
    return ESP_OK;
}

esp_err_t Korvo2MicCaptureBackend::setupPipeline() {
    if (pipelineRunning_) {
        return ESP_OK;
    }

    audio_pipeline_cfg_t pipelineCfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_ = audio_pipeline_init(&pipelineCfg);
    if (!pipeline_) {
        ESP_LOGE(kTag, "Failed to create audio pipeline");
        return ESP_ERR_NO_MEM;
    }

    i2s_stream_cfg_t i2sCfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
            CODEC_ADC_I2S_PORT, kSrcSampleRateHz, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_READER);
    i2sReader_ = i2s_stream_init(&i2sCfg);
    if (!i2sReader_) {
        ESP_LOGE(kTag, "Failed to create I2S reader");
        teardownPipeline();
        return ESP_ERR_NO_MEM;
    }

    rsp_filter_cfg_t rspCfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rspCfg.src_rate = kSrcSampleRateHz;
    rspCfg.src_ch = 2;
    rspCfg.src_bits = 16;
    rspCfg.dest_rate = kDestSampleRateHz;
    rspCfg.dest_ch = 2;
    rspCfg.dest_bits = 16;
    rspCfg.stack_in_ext = true;
    rspCfg.max_indata_bytes = 2048;
    resampler_ = rsp_filter_init(&rspCfg);
    if (!resampler_) {
        ESP_LOGE(kTag, "Failed to create resampler");
        teardownPipeline();
        return ESP_ERR_NO_MEM;
    }

    raw_stream_cfg_t rawCfg = RAW_STREAM_CFG_DEFAULT();
    rawCfg.type = AUDIO_STREAM_READER;
    rawCfg.out_rb_size = 8 * 1024;
    rawReader_ = raw_stream_init(&rawCfg);
    if (!rawReader_) {
        ESP_LOGE(kTag, "Failed to create raw reader");
        teardownPipeline();
        return ESP_ERR_NO_MEM;
    }

    audio_pipeline_register(pipeline_, i2sReader_, "i2s");
    audio_pipeline_register(pipeline_, resampler_, "filter");
    audio_pipeline_register(pipeline_, rawReader_, "raw");

    const char *linkTags[3] = {"i2s", "filter", "raw"};
    esp_err_t ret = audio_pipeline_link(pipeline_, linkTags, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to link pipeline: %s", esp_err_to_name(ret));
        teardownPipeline();
        return ret;
    }

    ret = audio_pipeline_run(pipeline_);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "Failed to run pipeline: %s", esp_err_to_name(ret));
        teardownPipeline();
        return ret;
    }

    // ADF WWE selects Korvo-2 mics after the I2S pipeline is running.
    es7210_mic_select(static_cast<es7210_input_mics_t>(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3));

    pipelineRunning_ = true;
    ESP_LOGI(kTag, "Mic capture pipeline running (%d Hz mono)", kDestSampleRateHz);
    return ESP_OK;
}

void Korvo2MicCaptureBackend::teardownPipeline() {
    if (pipeline_) {
        if (pipelineRunning_) {
            audio_pipeline_stop(pipeline_);
            audio_pipeline_wait_for_stop(pipeline_);
            audio_pipeline_terminate(pipeline_);
            pipelineRunning_ = false;
        }
        if (i2sReader_) {
            audio_pipeline_unregister(pipeline_, i2sReader_);
        }
        if (resampler_) {
            audio_pipeline_unregister(pipeline_, resampler_);
        }
        if (rawReader_) {
            audio_pipeline_unregister(pipeline_, rawReader_);
        }
        audio_pipeline_deinit(pipeline_);
        pipeline_ = nullptr;
    }
    if (i2sReader_) {
        audio_element_deinit(i2sReader_);
        i2sReader_ = nullptr;
    }
    if (resampler_) {
        audio_element_deinit(resampler_);
        resampler_ = nullptr;
    }
    if (rawReader_) {
        audio_element_deinit(rawReader_);
        rawReader_ = nullptr;
    }
}

void Korvo2MicCaptureBackend::captureTask(void *arg) {
    auto *self = static_cast<Korvo2MicCaptureBackend *>(arg);
    int16_t stereoBuffer[kStereoSamplesPerFrame];
    int16_t monoBuffer[kMonoSamplesPerFrame];
    uint32_t diagCounter = 0;

    vTaskDelay(pdMS_TO_TICKS(500));

    while (self->running_) {
        const int bytesRead = raw_stream_read(
                self->rawReader_,
                reinterpret_cast<char *>(stereoBuffer),
                static_cast<int>(sizeof(stereoBuffer)));
        if (bytesRead <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const size_t stereoSampleCount = static_cast<size_t>(bytesRead) / sizeof(int16_t);
        const size_t frameCount = stereoSampleCount / 2U;
        if (frameCount == 0 || !self->callback_) {
            continue;
        }

        if (++diagCounter == 1U || (diagCounter % 100U) == 0U) {
            uint32_t peakCh0 = 0;
            uint32_t peakCh1 = 0;
            for (size_t i = 0; i < frameCount; ++i) {
                const int32_t ch0 = stereoBuffer[i * 2U];
                const int32_t ch1 = stereoBuffer[(i * 2U) + 1U];
                const uint32_t absCh0 = static_cast<uint32_t>(ch0 < 0 ? -ch0 : ch0);
                const uint32_t absCh1 = static_cast<uint32_t>(ch1 < 0 ? -ch1 : ch1);
                if (absCh0 > peakCh0) {
                    peakCh0 = absCh0;
                }
                if (absCh1 > peakCh1) {
                    peakCh1 = absCh1;
                }
            }
            ESP_LOGI(kTag,
                     "Mic diag ch0=%lu ch1=%lu frames=%u",
                     static_cast<unsigned long>(peakCh0),
                     static_cast<unsigned long>(peakCh1),
                     static_cast<unsigned>(frameCount));
        }

        extractMonoFromStereo(stereoBuffer, frameCount, kMicChannelIndex, monoBuffer);
        self->callback_(monoBuffer, frameCount, esp_timer_get_time());
    }

    vTaskDelete(nullptr);
}

esp_err_t Korvo2MicCaptureBackend::start(MicAudioCallback callback) {
    if (running_) {
        return ESP_OK;
    }

    callback_ = std::move(callback);
    if (!callback_) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = setupPipeline();
    if (ret != ESP_OK) {
        callback_ = nullptr;
        return ret;
    }

    running_ = true;
    const BaseType_t created = xTaskCreate(
            captureTask,
            "korvo2_mic",
            kCaptureTaskStackWords,
            this,
            kCaptureTaskPriority,
            &captureTask_);
    if (created != pdPASS) {
        running_ = false;
        callback_ = nullptr;
        teardownPipeline();
        ESP_LOGE(kTag, "Failed to create capture task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "Korvo-2 onboard mic capture started");
    return ESP_OK;
}

void Korvo2MicCaptureBackend::stop() {
    if (!running_ && !pipelineRunning_) {
        callback_ = nullptr;
        return;
    }

    running_ = false;
    if (captureTask_) {
        for (int i = 0; i < 100; ++i) {
            if (eTaskGetState(captureTask_) == eDeleted) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        captureTask_ = nullptr;
    }
    callback_ = nullptr;
    teardownPipeline();
    ESP_LOGI(kTag, "Korvo-2 onboard mic capture stopped");
}

}  // namespace btspeaker
