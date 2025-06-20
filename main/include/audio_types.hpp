//
// Created by Shaked Melman on 18/06/2025.
//
#pragma once

#ifndef BT_SPEAKER_AUDIO_TYPES_HPP
#define BT_SPEAKER_AUDIO_TYPES_HPP

#endif //BT_SPEAKER_AUDIO_TYPES_HPP

#include "esp_raii_wrapper.hpp"

namespace btspeaker {

    enum class AudioSource {
        NONE,
        VOICE_RESPONSE,
        BLUETOOTH,
        SPOTIFY
    };

    enum class PlaybackState {
        STOPPED,
        PLAYING,
        PAUSED,
        PREPARING,
        ERROR
    };

    struct I2SAudioPipeline {
        PipelineHandle pipeline;
        ElementHandle i2SStreamWriter;
        ElementHandle i2SStreamReader;
        EventIfaceHandle evtHandle;
    };
}