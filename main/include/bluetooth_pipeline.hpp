#pragma once

#include "esp_raii_wrapper.hpp"
#include "audio_pipeline_util.hpp"
#include "base_audio_pipeline.hpp"
#include <cstring>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"
}

namespace btspeaker {

    class BluetoothPipeline : public BaseAudioPipeline {
    public:
        BluetoothPipeline();

        ~BluetoothPipeline() override = default;

        void setupPipeline() override;
        void cleanupPipeline() override;

    private:
        static constexpr const char *TAG = "BT_PIPELINE";
        static constexpr const int SAMPLE_RATE = 44100;
        static constexpr const int CHANNELS = 2;
        static constexpr const int BITS_PER_SAMPLE = 16;
        bool btServiceStarted = false;
    };

} // namespace btspeaker 