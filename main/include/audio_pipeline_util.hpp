#pragma once

#include "esp_raii_wrapper.hpp"
#include "audio_types.hpp"
#include <memory>

extern "C" {
#include "audio_element.h"
#include "i2s_stream.h"
}

namespace btspeaker {

    class AudioPipelineUtil {
    public:

        static  std::unique_ptr<I2SAudioPipeline> createAudioPipeline(AudioSource source,
                                                    audio_element_handle_t reader,
                                                    bool isInput = false,
                                                    int sampleRate = 44100,
                                                    int channels = 2,
                                                    int bitsPerSample = 16);

        static void stopPipeline(I2SAudioPipeline &audioPipeline);
        static void cleanupPipeline(I2SAudioPipeline &audioPipeline);

    private:
        AudioPipelineUtil() = default;

        ~AudioPipelineUtil() = default;

        // Internal I2S management
        static i2s_stream_cfg_t createI2SConfig(AudioSource source, int sampleRate, int channels, int bitsPerSample);

        static audio_element_handle_t createI2SWriter(AudioSource source,
                                                      int sample_rate = 44100,
                                                      int channels = 2,
                                                      int bits_per_sample = 16);

        static void registerAndLinkPipeline(I2SAudioPipeline &audioPipeline);

        // Constants
        static constexpr const char *TAG = "I2S_AUDIO_SERVICE";
        static constexpr int I2S_PORT = I2S_NUM_0;
        static constexpr uint8_t DEFAULT_VOLUME = 100;
    };

} // namespace btspeaker 