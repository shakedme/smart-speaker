#pragma once

#include <cstddef>
#include <cstdint>

namespace cspot {

struct PlaybackDecodeSnapshot {
    uint32_t decoded_rate_bps = 0;
    uint32_t cdn_rate_bps = 0;
    uint32_t decode_calls = 0;
    uint32_t gap_max_ms = 0;
    uint32_t decode_wall_ms_avg = 0;
    uint32_t decode_wall_ms_max = 0;
    uint32_t cdn_in_decode_ms_avg = 0;
    uint32_t cdn_in_decode_ms_max = 0;
    uint32_t cpu_in_decode_ms_avg = 0;
    uint32_t cpu_in_decode_ms_max = 0;
    uint32_t cdn_read_ms_avg = 0;
    uint32_t cdn_read_ms_max = 0;
    uint32_t cdn_reads = 0;
    uint32_t cdn_zero_reads = 0;
    uint32_t cdn_range_requests = 0;
    uint32_t cdn_range_failures = 0;
};

struct PlaybackFeedSnapshot {
    uint32_t feed_rate_bps = 0;
    uint32_t rb_block_count = 0;
    uint32_t feed_short_writes = 0;
};

void publishDecodeSnapshot(const PlaybackDecodeSnapshot &snapshot);

void publishFeedSnapshot(const PlaybackFeedSnapshot &snapshot);

class TrackPlayer;

void registerTrackPlayerForStallDiagnostics(TrackPlayer *player);

void flushDecodeDiagnosticsBeforeStall();

void logPlaybackStallVerdict(const char *tag,
                              size_t fill_bytes,
                              size_t capacity_bytes,
                              bool sink_playing,
                              uint32_t pause_low_water_bytes,
                              uint32_t recovery_prebuffer_bytes,
                              const char *trigger);

}  // namespace cspot
