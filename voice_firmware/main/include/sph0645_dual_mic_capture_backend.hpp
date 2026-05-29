#pragma once

#include "mic_capture_backend.hpp"

namespace btspeaker {

class Sph0645DualMicCaptureBackend final : public MicCaptureBackend {
public:
    const char *name() const override;
    esp_err_t init() override;
    esp_err_t start(MicAudioCallback callback) override;
    void stop() override;

private:
    static constexpr int kBclkGpio = 32;
    static constexpr int kWsGpio = 33;
    static constexpr int kDinGpio = 34;
    MicAudioCallback callback_;
};

}  // namespace btspeaker
