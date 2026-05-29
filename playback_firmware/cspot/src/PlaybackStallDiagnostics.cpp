#include "PlaybackStallDiagnostics.h"
#include "TrackPlayer.h"
#include "diagnostics_config.hpp"

#include <esp_log.h>

namespace cspot {

namespace {

TrackPlayer *g_track_player_for_stall = nullptr;

constexpr uint32_t kRealtimeStereoPcmBps = 176400;
constexpr uint32_t kSlowCdnReadMs = 200;
constexpr uint32_t kSlowGapMs = 500;
constexpr uint32_t kLowProducerRateBps = 150000;

PlaybackDecodeSnapshot g_decode_snapshot{};
PlaybackFeedSnapshot g_feed_snapshot{};

const char *bottleneckName(const char *verdict) {
    return verdict ? verdict : "UNKNOWN";
}

}  // namespace

void publishDecodeSnapshot(const PlaybackDecodeSnapshot &snapshot) {
    g_decode_snapshot = snapshot;
}

void publishFeedSnapshot(const PlaybackFeedSnapshot &snapshot) {
    g_feed_snapshot = snapshot;
}

void registerTrackPlayerForStallDiagnostics(TrackPlayer *player) {
    g_track_player_for_stall = player;
}

void flushDecodeDiagnosticsBeforeStall() {
    if (g_track_player_for_stall != nullptr) {
        g_track_player_for_stall->flushDiagnostics();
    }
}

void logPlaybackStallVerdict(const char *tag,
                              const size_t fill_bytes,
                              const size_t capacity_bytes,
                              const bool sink_playing,
                              const uint32_t pause_low_water_bytes,
                              const uint32_t recovery_prebuffer_bytes,
                              const char *trigger) {
    if (!btspeaker::diagnostics_enabled) {
        return;
    }

    const PlaybackDecodeSnapshot &d = g_decode_snapshot;
    const PlaybackFeedSnapshot &f = g_feed_snapshot;

    const char *verdict = "UNKNOWN";
    const char *detail = "";

    if (f.rb_block_count > 0 || f.feed_short_writes > 0) {
        verdict = "SINK_BACKPRESSURE";
        detail = "ringbuf blocked or short feed writes";
    } else if (d.decoded_rate_bps < kLowProducerRateBps &&
               d.cdn_rate_bps < 12000) {
        verdict = "CDN_IO";
        detail = "decoded and cdn rates below real-time";
    } else if (d.decoded_rate_bps < kLowProducerRateBps &&
               d.cdn_read_ms_max >= kSlowCdnReadMs) {
        if (d.decode_wall_ms_max > 0 &&
            d.cdn_in_decode_ms_max >= d.decode_wall_ms_max * 3 / 4) {
            verdict = "CDN_IO";
            detail = "decode wall time dominated by readBytes";
        } else if (d.cpu_in_decode_ms_avg > 25 &&
                   d.cdn_read_ms_max < kSlowCdnReadMs) {
            verdict = "DECODE_CPU";
            detail = "cpu time inside vorbis read without slow cdn";
        } else {
            verdict = "CDN_IO";
            detail = "low decode rate with slow cdn reads";
        }
    } else if (d.gap_max_ms >= kSlowGapMs &&
               d.cdn_read_ms_max >= kSlowCdnReadMs) {
        verdict = "CDN_IO";
        detail = "inter-decode gap aligns with slow cdn read";
    } else if (d.gap_max_ms >= kSlowGapMs &&
               d.cdn_read_ms_max < 50 &&
               d.decode_calls > 0) {
        verdict = "SCHEDULING";
        detail = "large gap without cdn read activity";
    } else if (fill_bytes < recovery_prebuffer_bytes &&
               d.decoded_rate_bps >= kRealtimeStereoPcmBps) {
        verdict = "BUFFER_POLICY";
        detail = "producer kept up; playback drained thin buffer";
    } else if (d.decoded_rate_bps >= kRealtimeStereoPcmBps) {
        verdict = "BUFFER_POLICY";
        detail = "producer rate ok at pause; likely thin-buffer playback";
    }

    const uint32_t cdn_pct = (d.decode_wall_ms_avg > 0)
                             ? (d.cdn_in_decode_ms_avg * 100U) / d.decode_wall_ms_avg
                             : 0;

    ESP_LOGW(tag,
             "[STALL_VERDICT] trigger=%s cause=%s %s fill=%u/%u playing=%d "
             "pause_below=%u recover_to=%u "
             "decoded=%luB/s cdn=%luB/s feed=%luB/s gap_max=%lu "
             "decode_wall_ms(avg=%lu max=%lu) cdn_in_decode(avg=%lu max=%lu) "
             "cpu_in_decode(avg=%lu max=%lu) cdn_pct=%lu "
             "cdn_read_ms(avg=%lu max=%lu) cdn_zero=%lu "
             "cdn_req=%lu cdn_fail=%lu "
             "rb_block=%lu short_feed=%lu",
             trigger ? trigger : "?",
             bottleneckName(verdict),
             detail,
             static_cast<unsigned>(fill_bytes),
             static_cast<unsigned>(capacity_bytes),
             sink_playing ? 1 : 0,
             static_cast<unsigned>(pause_low_water_bytes),
             static_cast<unsigned>(recovery_prebuffer_bytes),
             static_cast<unsigned long>(d.decoded_rate_bps),
             static_cast<unsigned long>(d.cdn_rate_bps),
             static_cast<unsigned long>(f.feed_rate_bps),
             static_cast<unsigned long>(d.gap_max_ms),
             static_cast<unsigned long>(d.decode_wall_ms_avg),
             static_cast<unsigned long>(d.decode_wall_ms_max),
             static_cast<unsigned long>(d.cdn_in_decode_ms_avg),
             static_cast<unsigned long>(d.cdn_in_decode_ms_max),
             static_cast<unsigned long>(d.cpu_in_decode_ms_avg),
             static_cast<unsigned long>(d.cpu_in_decode_ms_max),
             static_cast<unsigned long>(cdn_pct),
             static_cast<unsigned long>(d.cdn_read_ms_avg),
             static_cast<unsigned long>(d.cdn_read_ms_max),
             static_cast<unsigned long>(d.cdn_zero_reads),
             static_cast<unsigned long>(d.cdn_range_requests),
             static_cast<unsigned long>(d.cdn_range_failures),
             static_cast<unsigned long>(f.rb_block_count),
             static_cast<unsigned long>(f.feed_short_writes));
}

}  // namespace cspot
