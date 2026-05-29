#pragma once

#include "mic_capture_backend.hpp"

extern "C" {
#include "audio_element.h"
#include "audio_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

class Korvo2MicCaptureBackend final : public MicCaptureBackend {
public:
    ~Korvo2MicCaptureBackend() override;

    const char *name() const override;
    esp_err_t init() override;
    esp_err_t start(MicAudioCallback callback) override;
    void stop() override;

private:
    static void captureTask(void *arg);

    esp_err_t setupPipeline();
    void teardownPipeline();

    MicAudioCallback callback_;
    audio_pipeline_handle_t pipeline_ = nullptr;
    audio_element_handle_t i2sReader_ = nullptr;
    audio_element_handle_t resampler_ = nullptr;
    audio_element_handle_t rawReader_ = nullptr;
    TaskHandle_t captureTask_ = nullptr;
    volatile bool running_ = false;
    bool pipelineRunning_ = false;
};

}  // namespace btspeaker
