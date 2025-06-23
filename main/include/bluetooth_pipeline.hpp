#pragma once

#include "esp_raii_wrapper.hpp"
#include "audio_pipeline_util.hpp"
#include "base_audio_pipeline.hpp"
#include <cstring>
#include <esp_avrc_api.h>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"
}

namespace btspeaker {

    using ConnectedCallback = std::function<void()>;

    class BluetoothPipeline : public BaseAudioPipeline {
    public:
        BluetoothPipeline();

        ~BluetoothPipeline() override = default;

        static void bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p_param);

        void setConnectedCallback(ConnectedCallback callback) {
            this->connectedCallback = std::move(callback);
        }

        void setupPipeline() override;

        void cleanupPipeline() override;

        void disconnectCurrentDevice();

    private:
        static constexpr const char *TAG = "BT_PIPELINE";
        static constexpr const int SAMPLE_RATE = 44100;
        static constexpr const int CHANNELS = 2;
        static constexpr const int BITS_PER_SAMPLE = 16;
        bool btServiceStarted = false;
        ConnectedCallback connectedCallback;
        static BluetoothPipeline *instance;
        bool btConnected;
        esp_bd_addr_t connectedDeviceAddr{};

    };

} // namespace btspeaker 