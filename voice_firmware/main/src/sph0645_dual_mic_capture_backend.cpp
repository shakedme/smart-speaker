#include "sph0645_dual_mic_capture_backend.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

const char *Sph0645DualMicCaptureBackend::name() const {
    return "sph0645_dual_i2s";
}

esp_err_t Sph0645DualMicCaptureBackend::init() {
    ESP_LOGI("MIC",
             "[MIC] mic backend=sph0645_dual_i2s available/configured but not active by default "
             "(bclk=%d ws=%d din=%d)",
             kBclkGpio,
             kWsGpio,
             kDinGpio);
    return ESP_OK;
}

esp_err_t Sph0645DualMicCaptureBackend::start(MicAudioCallback callback) {
    callback_ = std::move(callback);
    ESP_LOGI("MIC", "SPH0645 dual-I2S mic capture backend selected");
    return ESP_OK;
}

void Sph0645DualMicCaptureBackend::stop() {
    callback_ = nullptr;
}

}  // namespace btspeaker
