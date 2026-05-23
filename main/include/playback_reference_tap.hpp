#pragma once

#include "aec_ref_config.hpp"

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace btspeaker {

class PlaybackReferenceTap {
public:
    using SendFrameFn = std::function<bool(const int16_t *pcm,
                                           uint32_t num_samples,
                                           uint32_t sequence,
                                           int64_t timestamp_us)>;

    PlaybackReferenceTap();

    void setSendCallback(SendFrameFn fn);

    void setActive(bool active);

    void recordOutboundQueueDepth(uint32_t depth);

    // Non-blocking. Drops output frames when send callback returns false.
    void feedPlaybackPcm(const int16_t *samples,
                         size_t frame_count,
                         uint32_t sample_rate_hz,
                         uint8_t channel_count,
                         int64_t timestamp_us);

    static PlaybackReferenceTap *instance();

    void setInstance(PlaybackReferenceTap *tap);

private:
    static constexpr uint32_t kPhaseFractionBits = 32;

    void resetResampler(uint32_t sample_rate_hz);

    void appendMonoSample(int16_t mono_sample);

    void flushOutputFrame(int64_t timestamp_us);

    void maybeLogStats(int64_t now_us);

    SendFrameFn send_fn_;
    std::atomic<bool> active_{false};

    uint32_t input_sample_rate_hz_{0};
    uint64_t phase_{0};
    uint64_t phase_step_{0};
    int16_t prev_mono_{0};
    bool have_prev_mono_{false};

    int16_t frame_buf_[kPlaybackRefFrameSamples]{};
    uint32_t frame_fill_{0};
    uint32_t sequence_{0};
    int64_t frame_timestamp_us_{0};
    int64_t next_output_timestamp_us_{0};
    uint32_t output_timestamp_remainder_{0};
    bool output_timestamp_valid_{false};

    int64_t stats_window_start_us_{0};
    uint32_t stats_frames_produced_{0};
    uint32_t stats_frames_dropped_{0};
    uint32_t stats_max_queue_depth_{0};
    uint64_t stats_sum_process_us_{0};
    uint32_t stats_process_calls_{0};
    uint32_t stats_input_rate_hz_{0};

    static PlaybackReferenceTap *instance_;
};

}  // namespace btspeaker

#endif  // CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
