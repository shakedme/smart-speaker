//
// Created by Shaked Melman on 17/06/2025.
//
#pragma once

#ifndef BT_SPEAKER_ADF_AUDIO_SINK_HPP
#define BT_SPEAKER_ADF_AUDIO_SINK_HPP

#include "AudioSink.h"
#include "audio_types.hpp"
#include "spotify_config.hpp"
#include <atomic>
#include <memory>

extern "C" {
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "raw_stream.h"
}

namespace btspeaker {


    class ADFAudioSink : public AudioSink {
    public:
        ADFAudioSink();

        size_t feedPCMFrames(const uint8_t *buffer, size_t bytes) override;

        ~ADFAudioSink() override;

        // Helper methods
        void setupPipeline();

        void prepare();

        void cleanupPipeline();

        void start();

        void resume() override;

        void pause() override;

        void flush() override;

        bool isPlaying() override {
            return isPlaying_.load();
        }

        bool isInitialized() {
            return initialized_.load();
        }

        size_t getBufferFillLevel() override;

        size_t getBufferCapacity() override;

    private:
        static constexpr const char *TAG = "ADF_AUDIO_SINK";
        // Must exceed EspPlayer::kInitialPreBufferBytes (1 MB) plus low-water headroom.
        static constexpr size_t kRawRingBufferSize = 1280 * 1024;
        static constexpr size_t kWriteChunkSize = 8192;

        std::unique_ptr<I2SAudioPipeline> audioPipeline;

        // Pipeline state
        std::atomic<bool> pipelineRunning_;
        std::atomic<bool> initialized_;
        std::atomic<bool> isPlaying_ = false;

        void maybeLogPlaybackDiagnostics();

        void maybeLogResumeFollowUp();

        std::atomic<uint32_t> ringbufBlockCount_{0};
        std::atomic<uint32_t> ringbufBlockMaxWaitMs_{0};
        std::atomic<uint32_t> feedShortWriteCount_{0};
        std::atomic<size_t> minFillBytes_{SIZE_MAX};
        int64_t lastDiagLogUs_{0};
        int64_t resumeDiagStartUs_{0};
        bool resumeDiag500Logged_{false};
        bool resumeDiag2000Logged_{false};
    };

} // namespace btspeaker


#endif //BT_SPEAKER_ADF_AUDIO_SINK_HPP