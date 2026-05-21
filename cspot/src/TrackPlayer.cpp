#include "TrackPlayer.h"
#include "cspot_task_config.h"
#include "diagnostics_config.hpp"

#include <mutex>        // for mutex, scoped_lock
#include <cstdio>
#include <string>       // for string
#include <type_traits>  // for remove_extent_t
#include <vector>       // for vector, vector<>::value_type

#include "BellLogger.h"  // for AbstractLogger
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "CSpotContext.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}
#include "EventManager.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "PlayPathProfiler.h"
#include "PlaybackStallDiagnostics.h"
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#include <esp_log.h>

#ifndef CONFIG_BELL_NOCODEC
#ifdef BELL_VORBIS_FLOAT
#define VORBIS_SEEK(file, position) \
  (ov_time_seek(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#endif
#endif

namespace cspot {
    struct Context;
    struct TrackReference;

    class PlaybackMetrics;
}  // namespace cspot

using namespace cspot;

namespace {
    constexpr uint32_t kSpotifyDecodeDiagIntervalMs = 5000;
}

#ifndef CONFIG_BELL_NOCODEC

static size_t vorbisReadCb(void *ptr, size_t size, size_t nmemb,
                           TrackPlayer *self) {
    return self->_vorbisRead(ptr, size, nmemb);
}

static int vorbisCloseCb(TrackPlayer *self) {
    return self->_vorbisClose();
}

static int vorbisSeekCb(TrackPlayer *self, int64_t offset, int whence) {

    return self->_vorbisSeek(offset, whence);
}

static long vorbisTellCb(TrackPlayer *self) {
    return self->_vorbisTell();
}

#endif

TrackPlayer::TrackPlayer(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::TrackQueue> trackQueue,
                         TrackEndedCallback onTrackEnd,
                         TrackChangedCallback onTrackChanged,
                         bool *repeating_track)
        : bell::Task("cspot_player", 60 * 1024, 5, CSPOT_PLAYER_CORE) {
    this->ctx = ctx;
    this->onTrackEnd = onTrackEnd;
    this->onTrackChanged = onTrackChanged;
    this->trackQueue = trackQueue;
    this->playbackSemaphore = std::make_unique<bell::WrappedSemaphore>(5);
    repeating_track_ = repeating_track;

#ifndef CONFIG_BELL_NOCODEC
    // Initialize vorbis callbacks
    vorbisFile = {};
    vorbisCallbacks = {
            (decltype(ov_callbacks::read_func)) &vorbisReadCb,
            (decltype(ov_callbacks::seek_func)) &vorbisSeekCb,
            (decltype(ov_callbacks::close_func)) &vorbisCloseCb,
            (decltype(ov_callbacks::tell_func)) &vorbisTellCb,
    };
#endif
    registerTrackPlayerForStallDiagnostics(this);
}

TrackPlayer::~TrackPlayer() {
    registerTrackPlayerForStallDiagnostics(nullptr);
    isRunning = false;
    resetState();
    std::scoped_lock lock(runningMutex);
}

void TrackPlayer::flushDiagnostics() {
    flushDecodeDiagnosticsWindow();
}

void TrackPlayer::start() {
    if (!isRunning) {
        isRunning = true;
        bool taskStarted = startTask();
        if (!taskStarted) {
            isRunning = false;
            return;
        }
        this->ctx->playbackMetrics->start_reason = PlaybackMetrics::REMOTE;
        this->ctx->playbackMetrics->start_source = "unknown";
    } else {
        ESP_LOGI("TrackPlayer", "TrackPlayer already running");
        this->ctx->playbackMetrics->end_reason = PlaybackMetrics::END_PLAY;
    }
}

void TrackPlayer::stop() {
    isRunning = false;
    resetState();
    std::scoped_lock lock(runningMutex);
}

void TrackPlayer::resetState(bool paused) {
    // Mark for reset
    this->pendingReset = true;
    this->currentSongPlaying = false;
    this->startPaused = paused;

    std::scoped_lock lock(dataOutMutex);

    ESP_LOGI("TrackPlayer", "Resetting state");
    PlayPathProfiler::mark("track_player.reset_state");
}

void TrackPlayer::seekMs(size_t ms, bool loading) {
#ifndef CONFIG_BELL_NOCODEC
    if (!loading) {
        // We're in the middle of the next track, so we need to reset the player in order to seek
        resetState();
    }
#endif

    this->pendingSeekPositionMs = ms;
}

void TrackPlayer::runTask() {
    std::scoped_lock lock(runningMutex);

    std::shared_ptr<QueuedTrack> track = nullptr, newTrack = nullptr;

    int trackOffset = 0;
    size_t tracksPlayed = 1;
    bool eof = false;
    bool endOfQueueReached = false;

    while (isRunning) {
        bool properStream = true;
        PlayPathProfiler::mark("track_player.wait_playable");
        this->trackQueue->playableSemaphore->wait();
        PlayPathProfiler::mark("track_player.playable_received");

        // Last track was interrupted, reset to default
        if (pendingReset) {
            track = nullptr;
            pendingReset = false;
            inFuture = false;
        }

        endOfQueueReached = false;

        // Wait 800ms. If next reset is requested in meantime, restart the queue.
        // Gets rid of excess actions during rapid queueing
        BELL_SLEEP_MS(20);

        if (pendingReset) {
            continue;
        }
        if (!*repeating_track_ || newTrack == nullptr)
            newTrack = trackQueue->consumeTrack(track, trackOffset);

        if (newTrack == nullptr) {
            if (trackOffset == -1) {
                // Reset required
                track = nullptr;
            }
            PlayPathProfiler::mark("track_player.consume_empty");
            BELL_SLEEP_MS(20);
            continue;
        }

        {
            char detail[64];
            snprintf(detail, sizeof(detail), "track=%s state=%d",
                     PlayPathProfiler::shortTrackRef(newTrack->ref.uri),
                     (int) newTrack->state);
            PlayPathProfiler::mark("track_player.consume_track", detail);
        }

        track = newTrack;
        this->ctx->playbackMetrics->trackMetrics = track->trackMetrics;

        inFuture = trackOffset > 0;
        uint8_t retries = 10;
        const uint32_t readyWaitStartMs = esp_log_timestamp();
        while (track->state != QueuedTrack::State::READY &&
               track->state != QueuedTrack::State::FAILED && retries-- > 0) {
            BELL_SLEEP_MS(20);
        }
        if (track->state != QueuedTrack::State::READY) {
            char detail[64];
            snprintf(detail, sizeof(detail), "state=%d waited_ms=%u",
                     (int) track->state,
                     (unsigned) (esp_log_timestamp() - readyWaitStartMs));
            PlayPathProfiler::mark("track_player.ready_wait_failed", detail);
            ESP_LOGE("TrackPlayer", "Track failed to load, final state: %i, skipping it", (int) track->state);
            if (track->ref.removed != NULL)
                this->onTrackChanged(track, false);
            this->onTrackEnd(true);
            continue;
        }
        {
            char detail[64];
            snprintf(detail, sizeof(detail), "track=%s waited_ms=%u",
                     PlayPathProfiler::shortTrackRef(track->ref.uri),
                     (unsigned) (esp_log_timestamp() - readyWaitStartMs));
            PlayPathProfiler::mark("track_player.ready_confirmed", detail);
        }
        
        track->playingTrackIndex = tracksPlayed;
        currentSongPlaying = true;
        track->trackMetrics->startTrack();
        retries = 3;
        {
            std::scoped_lock lock(playbackMutex);
            bool skipped = 0;

            currentTrackStream = track->getAudioFile();
            // Open the stream
#ifndef CONFIG_BELL_NOCODEC
            if (currentTrackStream && !currentTrackStream->isStreamOpen()) {
                PlayPathProfiler::mark("track_player.open_stream_begin",
                                       PlayPathProfiler::shortTrackRef(track->ref.uri));
                currentTrackStream->openStream();
                PlayPathProfiler::mark("track_player.open_stream_done",
                                       PlayPathProfiler::shortTrackRef(track->ref.uri));
            } else if (currentTrackStream && currentTrackStream->isStreamOpen()) {
                PlayPathProfiler::mark("track_player.open_stream_skip",
                                       "already_warmed");
            }
#else
            ssize_t start_offset = 0;
            uint8_t* headerBuf = nullptr;
            if (currentTrackStream && !currentTrackStream->isStreamOpen()) {
                PlayPathProfiler::mark("track_player.open_stream_begin",
                                       PlayPathProfiler::shortTrackRef(track->ref.uri));
                headerBuf = currentTrackStream->openStream(start_offset);
                PlayPathProfiler::mark("track_player.open_stream_done",
                                       PlayPathProfiler::shortTrackRef(track->ref.uri));
            } else if (currentTrackStream && currentTrackStream->isStreamOpen()) {
                PlayPathProfiler::mark("track_player.open_stream_skip",
                                       "already_warmed");
                start_offset = static_cast<ssize_t>(currentTrackStream->getPosition());
                headerBuf = currentTrackStream->openStream(start_offset);
            }
            if (start_offset < 0) {
              ESP_LOGE("TrackPlayer", "Track failed to load, skipping it");
              this->onTrackEnd(true);
              continue;
            }
#endif
            if (pendingReset || !currentSongPlaying) {
                continue;
            }
            track->trackMetrics->startTrackDecoding();
            track->trackMetrics->track_size = currentTrackStream->getSize();

            this->onTrackChanged(track, true);
            PlayPathProfiler::mark("track_player.decoding_started",
                                   PlayPathProfiler::shortTrackRef(track->ref.uri));
            startPaused = false;

#ifndef CONFIG_BELL_NOCODEC
            int32_t r =
                    ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);
#else
            size_t toWrite = start_offset;
            while (toWrite) {
              size_t written = dataCallback(headerBuf + (start_offset - toWrite),
                                            toWrite, tracksPlayed, 0);
              if (written == 0) {
                vTaskDelay(1);
              }
              toWrite -= written;
            }

            track->written_bytes += start_offset;
            float duration_lambda = 1.0 *
                                    (currentTrackStream->getSize() - start_offset) /
                                    track->trackInfo.duration;
#endif
            if (pendingSeekPositionMs > 0) {
                track->requestedPosition = pendingSeekPositionMs;
#ifdef CONFIG_BELL_NOCODEC
                pendingSeekPositionMs = 0;
#endif
            }
            ctx->playbackMetrics->end_reason = PlaybackMetrics::REMOTE;

            if (track->requestedPosition > 0) {
#ifndef CONFIG_BELL_NOCODEC
                VORBIS_SEEK(&vorbisFile, track->requestedPosition);
#else
                size_t seekPosition =
                    track->requestedPosition * duration_lambda + start_offset;
                currentTrackStream->seek(seekPosition);
                skipped = true;
#endif
            }

            eof = false;
            track->loading = true;

            while (!eof && currentSongPlaying) {
                // Execute seek if needed
                if (pendingSeekPositionMs > 0) {
                    track->requestedPosition = pendingSeekPositionMs;

                    // Seek to the new position
#ifndef CONFIG_BELL_NOCODEC
                    VORBIS_SEEK(&vorbisFile, track->requestedPosition);
#else
                    uint32_t seekPosition = track->requestedPosition * duration_lambda +
                                            seekable_callback(tracksPlayed);
                    currentTrackStream->seek(seekPosition);
                    skipped = true;
#endif
                    track->trackMetrics->newPosition(pendingSeekPositionMs);
                    // Reset the pending seek position
                    pendingSeekPositionMs = 0;
                    this->onTrackChanged(track, false);
                }

                insideVorbisRead_ = true;
                currentReadCdnMs_ = 0;
                const uint32_t decodeStartMs = esp_log_timestamp();
                long ret =
#ifdef CONFIG_BELL_NOCODEC
                        this->currentTrackStream->readBytes(&pcmBuffer[0],
                                                            pcmBuffer.size());
#else
                        VORBIS_READ(&vorbisFile, (char *) &pcmBuffer[0], pcmBuffer.size(),
                                    &currentSection);
#endif
                insideVorbisRead_ = false;
                const uint32_t decodeMs = esp_log_timestamp() - decodeStartMs;
                const uint32_t cdnInDecodeMs = currentReadCdnMs_;
                const uint32_t cpuInDecodeMs =
                        decodeMs > cdnInDecodeMs ? decodeMs - cdnInDecodeMs : 0;
                recordDecodeDiagnostics(ret, decodeMs, cdnInDecodeMs);
                (void) cpuInDecodeMs;

                if (ret < 0) {
                    if (retries == 0) {
                        currentSongPlaying = false;
                        properStream = false;
                        eof = true;
                    } else {
                        ESP_LOGE("TrackPlayer", "An error has occured in the stream");
                        retries--;
                        ESP_LOGE("TrackPlayer", "Retries left:%d", retries);
#ifndef CONFIG_BELL_NOCODEC
                        ov_clear(&vorbisFile);
#endif
                        size_t pos = this->currentTrackStream->getPosition();

                        this->currentTrackStream = nullptr;
                        this->currentTrackStream = track->getAudioFile();

                        // Open the stream
#ifndef CONFIG_BELL_NOCODEC
                        currentTrackStream->openStream();
                        int32_t r =
                                ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);
#else
                        ssize_t start_offset = 0;
                        uint8_t* headerBuf = currentTrackStream->openStream(start_offset);
                        if (start_offset < 0) {
                          ESP_LOGE("TrackPlayer", "Track failed to reload, skipping it");
                          currentSongPlaying = false;
                          properStream = false;
                          eof = true;

                        } else
#endif
                        this->currentTrackStream->seek(pos);
                    }
                } else {
                    if (ret == 0) {
                        // and done :)
                        eof = true;
                    }
                    if (this->dataCallback != nullptr) {
                        auto toWrite = ret;

                        uint32_t zeroWriteStreak = 0;
                        while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {
                            int written = 0;
                            {
                                std::scoped_lock dataOutLock(dataOutMutex);
                                // If reset happened during playback, return
                                if (!currentSongPlaying || pendingReset)
                                    break;
#ifdef CONFIG_BELL_NOCODEC
                                if (skipped) {
                                  // Reset the pending seek position
                                  skipped = false;
                                }
#endif
                                written = dataCallback(pcmBuffer.data() + (ret - toWrite),
                                                       toWrite, tracksPlayed, skipped);
                            }
                            toWrite -= written;
                            if (written == 0) {
                                zeroWriteStreak++;
                                const TickType_t delayTicks =
                                        zeroWriteStreak >= 50
                                        ? pdMS_TO_TICKS(20)
                                        : pdMS_TO_TICKS(1);
                                vTaskDelay(delayTicks > 0 ? delayTicks : 1);
                                continue;
                            }
                            zeroWriteStreak = 0;
                        }
                        track->written_bytes += ret;
                    }
                }
            }
            // Only advance the PCM track id when a track finished naturally.
            // resetState() exits this loop with eof=false; incrementing then
            // makes EspPlayer think the track changed and can skip the queue.
            if (eof) {
                tracksPlayed++;
            }
#ifndef CONFIG_BELL_NOCODEC
            ov_clear(&vorbisFile);
#endif
            // always move back to LOADING (ensure proper seeking after last track has been loaded)
            currentTrackStream = nullptr;
            track->loading = false;
        }

        if (eof) {
            if (this->trackQueue->preloadedTracks.size() <= 1) {
                endOfQueueReached = true;
            }
#ifdef CONFIG_BELL_NOCODEC
            this->onTrackEnd(true);
#endif
        }
    }
}

#ifndef CONFIG_BELL_NOCODEC

size_t TrackPlayer::_vorbisRead(void *ptr, size_t size, size_t nmemb) {
    if (this->currentTrackStream == nullptr) {
        return 0;
    }
    const uint32_t readStartMs = esp_log_timestamp();
    const size_t bytesRead = this->currentTrackStream->readBytes((uint8_t *) ptr, nmemb * size);
    const uint32_t readMs = esp_log_timestamp() - readStartMs;
    recordCdnReadDiagnostics(bytesRead, readMs);
    return bytesRead;
}

size_t TrackPlayer::_vorbisClose() {
    return 0;
}

int TrackPlayer::_vorbisSeek(int64_t offset, int whence) {
    if (this->currentTrackStream == nullptr) {
        return 0;
    }
    switch (whence) {
        case 0:
            this->currentTrackStream->seek(offset);  // Spotify header offset
            break;
        case 1:
            this->currentTrackStream->seek(this->currentTrackStream->getPosition() +
                                           offset);
            break;
        case 2:
            this->currentTrackStream->seek(this->currentTrackStream->getSize() +
                                           offset);
            break;
    }

    return 0;
}

long TrackPlayer::_vorbisTell() {
    if (this->currentTrackStream == nullptr) {
        return 0;
    }
    return this->currentTrackStream->getPosition();
}

#endif

void TrackPlayer::setDataCallback(DataCallback callback,
                                  SeekableCallback seekable_callback,
                                  SeekableCallback spaces_available) {
    this->dataCallback = callback;
#ifdef CONFIG_BELL_NOCODEC
    this->seekable_callback = seekable_callback;
    this->spaces_available = spaces_available;
#endif
}

void TrackPlayer::recordDecodeDiagnostics(const long decodedBytes,
                                          const uint32_t decodeWallMs,
                                          const uint32_t cdnInDecodeMs) {
    if (!btspeaker::diagnostics_enabled) {
        return;
    }

    const int64_t nowUs = static_cast<int64_t>(esp_log_timestamp());
    if (decodeDiagWindowStartUs_ == 0) {
        decodeDiagWindowStartUs_ = nowUs;
    }
    if (decodeDiagLastCallUs_ != 0) {
        const uint32_t gapMs = static_cast<uint32_t>(nowUs - decodeDiagLastCallUs_);
        if (gapMs > decodeDiagMaxGapMs_) {
            decodeDiagMaxGapMs_ = gapMs;
        }
    }
    decodeDiagLastCallUs_ = nowUs;

    decodeDiagCalls_++;
    decodeDiagSumDecodeUs_ += decodeWallMs;
    if (decodeWallMs > decodeDiagMaxDecodeUs_) {
        decodeDiagMaxDecodeUs_ = decodeWallMs;
    }
    windowSumDecodeWallMs_ += decodeWallMs;
    windowSumCdnInDecodeMs_ += cdnInDecodeMs;
    const uint32_t cpuInDecodeMs =
            decodeWallMs > cdnInDecodeMs ? decodeWallMs - cdnInDecodeMs : 0;
    windowSumCpuInDecodeMs_ += cpuInDecodeMs;
    if (cdnInDecodeMs > windowMaxCdnInDecodeMs_) {
        windowMaxCdnInDecodeMs_ = cdnInDecodeMs;
    }
    if (cpuInDecodeMs > windowMaxCpuInDecodeMs_) {
        windowMaxCpuInDecodeMs_ = cpuInDecodeMs;
    }
    if (decodeWallMs > windowMaxDecodeWallMs_) {
        windowMaxDecodeWallMs_ = decodeWallMs;
    }
    if (decodedBytes > 0) {
        decodeDiagBytes_ += static_cast<uint64_t>(decodedBytes);
    } else if (decodedBytes == 0) {
        decodeDiagZeroReads_++;
    } else {
        decodeDiagErrors_++;
    }

    if ((nowUs - decodeDiagWindowStartUs_) < kSpotifyDecodeDiagIntervalMs) {
        return;
    }

    flushDecodeDiagnosticsWindow();
}

void TrackPlayer::flushDecodeDiagnosticsWindow() {
    if (!btspeaker::diagnostics_enabled) {
        return;
    }

    const int64_t nowUs = static_cast<int64_t>(esp_log_timestamp());
    const int64_t windowMs = nowUs - decodeDiagWindowStartUs_;
    if (windowMs <= 0) {
        return;
    }

    const uint32_t avgDecodeMs = decodeDiagCalls_ > 0
                                 ? static_cast<uint32_t>(decodeDiagSumDecodeUs_ /
                                                         decodeDiagCalls_)
                                 : 0;
    const uint32_t decodeRate = static_cast<uint32_t>(
            (decodeDiagBytes_ * 1000ULL) / static_cast<uint64_t>(windowMs));
    const uint32_t avgCdnReadMs = cdnDiagReads_ > 0
                                  ? static_cast<uint32_t>(cdnDiagSumReadUs_ /
                                                          cdnDiagReads_)
                                  : 0;
    const uint32_t cdnRate = static_cast<uint32_t>(
            (cdnDiagBytes_ * 1000ULL) / static_cast<uint64_t>(windowMs));
    const uint32_t avgCdnInDecodeMs = decodeDiagCalls_ > 0
                                      ? static_cast<uint32_t>(
                                                windowSumCdnInDecodeMs_ /
                                                decodeDiagCalls_)
                                      : 0;
    const uint32_t avgCpuInDecodeMs = decodeDiagCalls_ > 0
                                      ? static_cast<uint32_t>(
                                                windowSumCpuInDecodeMs_ /
                                                decodeDiagCalls_)
                                      : 0;

    ESP_LOGI("TrackPlayer",
             "[SPOTIFY_DECODE_DIAG] calls=%lu decoded=%lu rate=%luB/s zero=%lu err=%lu "
             "gap_max_ms=%lu decode_ms(avg=%lu max=%lu) cdn_in_decode_ms(avg=%lu max=%lu) "
             "cpu_in_decode_ms(avg=%lu max=%lu) cdn_reads=%lu cdn_bytes=%lu "
             "cdn_rate=%luB/s cdn_zero=%lu cdn_read_ms(avg=%lu max=%lu) "
             "cdn_req=%lu cdn_fail=%lu",
             static_cast<unsigned long>(decodeDiagCalls_),
             static_cast<unsigned long>(decodeDiagBytes_),
             static_cast<unsigned long>(decodeRate),
             static_cast<unsigned long>(decodeDiagZeroReads_),
             static_cast<unsigned long>(decodeDiagErrors_),
             static_cast<unsigned long>(decodeDiagMaxGapMs_),
             static_cast<unsigned long>(avgDecodeMs),
             static_cast<unsigned long>(decodeDiagMaxDecodeUs_),
             static_cast<unsigned long>(avgCdnInDecodeMs),
             static_cast<unsigned long>(windowMaxCdnInDecodeMs_),
             static_cast<unsigned long>(avgCpuInDecodeMs),
             static_cast<unsigned long>(windowMaxCpuInDecodeMs_),
             static_cast<unsigned long>(cdnDiagReads_),
             static_cast<unsigned long>(cdnDiagBytes_),
             static_cast<unsigned long>(cdnRate),
             static_cast<unsigned long>(cdnDiagZeroReads_),
             static_cast<unsigned long>(avgCdnReadMs),
             static_cast<unsigned long>(cdnDiagMaxReadUs_),
             static_cast<unsigned long>(cdnDiagRangeRequests_),
             static_cast<unsigned long>(cdnDiagRangeFailures_));

    PlaybackDecodeSnapshot snapshot{};
    snapshot.decoded_rate_bps = decodeRate;
    snapshot.cdn_rate_bps = cdnRate;
    snapshot.decode_calls = decodeDiagCalls_;
    snapshot.gap_max_ms = decodeDiagMaxGapMs_;
    snapshot.decode_wall_ms_avg = avgDecodeMs;
    snapshot.decode_wall_ms_max = windowMaxDecodeWallMs_;
    snapshot.cdn_in_decode_ms_avg = avgCdnInDecodeMs;
    snapshot.cdn_in_decode_ms_max = windowMaxCdnInDecodeMs_;
    snapshot.cpu_in_decode_ms_avg = avgCpuInDecodeMs;
    snapshot.cpu_in_decode_ms_max = windowMaxCpuInDecodeMs_;
    snapshot.cdn_read_ms_avg = avgCdnReadMs;
    snapshot.cdn_read_ms_max = cdnDiagMaxReadUs_;
    snapshot.cdn_reads = cdnDiagReads_;
    snapshot.cdn_zero_reads = cdnDiagZeroReads_;
    snapshot.cdn_range_requests = cdnDiagRangeRequests_;
    snapshot.cdn_range_failures = cdnDiagRangeFailures_;
    publishDecodeSnapshot(snapshot);

    decodeDiagWindowStartUs_ = nowUs;
    decodeDiagCalls_ = 0;
    decodeDiagZeroReads_ = 0;
    decodeDiagErrors_ = 0;
    decodeDiagMaxGapMs_ = 0;
    decodeDiagMaxDecodeUs_ = 0;
    decodeDiagBytes_ = 0;
    decodeDiagSumDecodeUs_ = 0;
    cdnDiagReads_ = 0;
    cdnDiagZeroReads_ = 0;
    cdnDiagMaxReadUs_ = 0;
    cdnDiagBytes_ = 0;
    cdnDiagSumReadUs_ = 0;
    cdnDiagRangeRequests_ = 0;
    cdnDiagRangeFailures_ = 0;
    windowSumDecodeWallMs_ = 0;
    windowSumCdnInDecodeMs_ = 0;
    windowSumCpuInDecodeMs_ = 0;
    windowMaxDecodeWallMs_ = 0;
    windowMaxCdnInDecodeMs_ = 0;
    windowMaxCpuInDecodeMs_ = 0;
}

void TrackPlayer::recordCdnReadDiagnostics(const size_t bytesRead, const uint32_t readMs) {
    if (!btspeaker::diagnostics_enabled) {
        return;
    }

    cdnDiagReads_++;
    cdnDiagBytes_ += bytesRead;
    cdnDiagSumReadUs_ += readMs;
    if (bytesRead == 0) {
        cdnDiagZeroReads_++;
    }
    if (readMs > cdnDiagMaxReadUs_) {
        cdnDiagMaxReadUs_ = readMs;
    }
    if (insideVorbisRead_) {
        currentReadCdnMs_ += readMs;
    }
    if (currentTrackStream) {
        const auto cdn = currentTrackStream->consumeDiagnostics();
        cdnDiagRangeRequests_ += cdn.rangeRequests;
        cdnDiagRangeFailures_ += cdn.rangeFailures;
    }
}
