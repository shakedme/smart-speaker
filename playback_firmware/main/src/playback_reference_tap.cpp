#include "../include/playback_reference_tap.hpp"

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM

#include <algorithm>
#include <cstring>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
}

namespace btspeaker {

namespace {
constexpr const char *kTag = "PLAYBACK_REF_TAP";
constexpr uint32_t kPhaseFractionBits = 32;
constexpr uint64_t kPhaseOne = 1ULL << kPhaseFractionBits;
}  // namespace

PlaybackReferenceTap *PlaybackReferenceTap::instance_ = nullptr;

PlaybackReferenceTap::PlaybackReferenceTap() = default;

void PlaybackReferenceTap::setInstance(PlaybackReferenceTap *tap) {
    instance_ = tap;
}

PlaybackReferenceTap *PlaybackReferenceTap::instance() {
    return instance_;
}

void PlaybackReferenceTap::setSendCallback(SendFrameFn fn) {
    send_fn_ = std::move(fn);
}

void PlaybackReferenceTap::setActive(const bool active) {
    active_.store(active, std::memory_order_release);
    if (!active) {
        frame_fill_ = 0;
        have_prev_mono_ = false;
        output_timestamp_valid_ = false;
        output_timestamp_remainder_ = 0;
    }
}

void PlaybackReferenceTap::recordOutboundQueueDepth(const uint32_t depth) {
    if (depth > stats_max_queue_depth_) {
        stats_max_queue_depth_ = depth;
    }
}

void PlaybackReferenceTap::resetResampler(const uint32_t sample_rate_hz) {
    if (sample_rate_hz == input_sample_rate_hz_ && phase_step_ != 0) {
        return;
    }

    input_sample_rate_hz_ = sample_rate_hz;
    stats_input_rate_hz_ = sample_rate_hz;
    phase_ = 0;
    have_prev_mono_ = false;
    output_timestamp_valid_ = false;
    output_timestamp_remainder_ = 0;

    phase_step_ = (static_cast<uint64_t>(sample_rate_hz) << kPhaseFractionBits) /
                  kPlaybackRefOutputSampleRate;
    if (phase_step_ == 0) {
        phase_step_ = 1;
    }

    ESP_LOGI(kTag, "Resampler configured: %lu Hz -> %lu Hz mono",
             static_cast<unsigned long>(sample_rate_hz),
             static_cast<unsigned long>(kPlaybackRefOutputSampleRate));
}

void PlaybackReferenceTap::appendMonoSample(const int16_t mono_sample) {
    if (frame_fill_ == 0) {
        frame_timestamp_us_ = next_output_timestamp_us_;
    }
    frame_buf_[frame_fill_++] = mono_sample;
    if (frame_fill_ >= kPlaybackRefFrameSamples) {
        flushOutputFrame(frame_timestamp_us_);
    }

    next_output_timestamp_us_ += 1000000 / kPlaybackRefOutputSampleRate;
    output_timestamp_remainder_ += 1000000 % kPlaybackRefOutputSampleRate;
    if (output_timestamp_remainder_ >= kPlaybackRefOutputSampleRate) {
        next_output_timestamp_us_++;
        output_timestamp_remainder_ -= kPlaybackRefOutputSampleRate;
    }
}

void PlaybackReferenceTap::flushOutputFrame(const int64_t timestamp_us) {
    if (frame_fill_ == 0) {
        return;
    }

    if (frame_fill_ < kPlaybackRefFrameSamples) {
        std::memset(frame_buf_ + frame_fill_, 0,
                    (kPlaybackRefFrameSamples - frame_fill_) * sizeof(int16_t));
    }

    stats_frames_produced_++;
    bool sent = false;
    if (send_fn_) {
        sent = send_fn_(frame_buf_, kPlaybackRefFrameSamples, sequence_++, timestamp_us);
    }
    if (!sent) {
        stats_frames_dropped_++;
    }

    frame_fill_ = 0;
}

void PlaybackReferenceTap::maybeLogStats(const int64_t now_us) {
    if (stats_window_start_us_ == 0) {
        stats_window_start_us_ = now_us;
        return;
    }

    if ((now_us - stats_window_start_us_) < (kPlaybackRefStatsIntervalMs * 1000LL)) {
        return;
    }

    const uint32_t avg_process_us = stats_process_calls_ > 0
                                    ? static_cast<uint32_t>(stats_sum_process_us_ /
                                                            stats_process_calls_)
                                    : 0;

    ESP_LOGI(kTag,
             "[PLAYBACK_REF_DIAG] produced=%lu dropped=%lu max_q=%lu in_rate=%lu "
             "proc_us(avg=%lu calls=%lu) active=%d",
             static_cast<unsigned long>(stats_frames_produced_),
             static_cast<unsigned long>(stats_frames_dropped_),
             static_cast<unsigned long>(stats_max_queue_depth_),
             static_cast<unsigned long>(stats_input_rate_hz_),
             static_cast<unsigned long>(avg_process_us),
             static_cast<unsigned long>(stats_process_calls_),
             active_.load(std::memory_order_relaxed) ? 1 : 0);

    stats_window_start_us_ = now_us;
    stats_frames_produced_ = 0;
    stats_frames_dropped_ = 0;
    stats_max_queue_depth_ = 0;
    stats_sum_process_us_ = 0;
    stats_process_calls_ = 0;
}

void PlaybackReferenceTap::feedPlaybackPcm(const int16_t *samples,
                                           const size_t frame_count,
                                           const uint32_t sample_rate_hz,
                                           const uint8_t channel_count,
                                           const int64_t timestamp_us) {
    if (!active_.load(std::memory_order_acquire) || !samples || frame_count == 0 ||
        channel_count == 0 || sample_rate_hz == 0) {
        return;
    }

    const int64_t block_start_us = esp_timer_get_time();
    resetResampler(sample_rate_hz);
    if (!output_timestamp_valid_ || frame_fill_ == 0) {
        next_output_timestamp_us_ = timestamp_us;
        output_timestamp_remainder_ = 0;
        output_timestamp_valid_ = true;
    }

    auto mono_at = [&](const size_t index) -> int32_t {
        if (channel_count >= 2) {
            const int32_t left = samples[index * channel_count];
            const int32_t right = samples[index * channel_count + 1];
            return (left + right) / 2;
        }
        return samples[index];
    };

    while (true) {
        const uint64_t whole = phase_ >> kPhaseFractionBits;
        if (whole >= frame_count) {
            break;
        }

        const uint32_t frac = static_cast<uint32_t>(phase_ & (kPhaseOne - 1));
        const int32_t s0 = mono_at(static_cast<size_t>(whole));
        const int32_t s1 = (whole + 1 < frame_count)
                           ? mono_at(static_cast<size_t>(whole + 1))
                           : (have_prev_mono_ ? prev_mono_ : s0);
        const int32_t interp =
                s0 + static_cast<int32_t>((static_cast<int64_t>(s1 - s0) * frac) >> kPhaseFractionBits);
        appendMonoSample(static_cast<int16_t>(
                std::max<int32_t>(-32768, std::min<int32_t>(32767, interp))));
        phase_ += phase_step_;
    }

    phase_ -= static_cast<uint64_t>(frame_count) << kPhaseFractionBits;

    if (frame_count > 0) {
        prev_mono_ = static_cast<int16_t>(mono_at(frame_count - 1));
        have_prev_mono_ = true;
    }

    const uint32_t process_us =
            static_cast<uint32_t>(esp_timer_get_time() - block_start_us);
    stats_sum_process_us_ += process_us;
    stats_process_calls_++;
    maybeLogStats(esp_timer_get_time());
}

}  // namespace btspeaker

#endif  // CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
