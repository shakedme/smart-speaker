#include "EspPlayer.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>
#include <esp_log.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#endif

#include "BellUtils.h"
#include "DeviceStateHandler.h"
#include "Logger.h"
#include "PlayPathProfiler.h"
#include "PlaybackStallDiagnostics.h"
#include "TrackPlayer.h"
#include "diagnostics_config.hpp"

static const char *kEspPlayerTag = "EspPlayer";

namespace {
    constexpr int64_t kSpotifyFeedDiagIntervalUs = 5 * 1000 * 1000;
}

EspPlayer::EspPlayer(std::shared_ptr<AudioSink> sink,
                     std::shared_ptr<cspot::DeviceStateHandler> handler)
        // Core 0 + elevated priority for timeline heartbeats; 24KB PSRAM stack
        // is required for putPlayerState/protobuf encoding.
        : bell::Task("esp_player", 24 * 1024, 4, 0) {
    this->handler = std::move(handler);
    this->audioSink = std::move(sink);

    this->handler->trackPlayer->setDataCallback(
            [this](uint8_t *data, size_t bytes, size_t trackId,
                   bool STORAGE_VOLATILE) {
                return this->feedData(data, bytes, trackId);
            });

    this->isPaused = false;

    this->handler->stateCallback =
            [this](cspot::DeviceStateHandler::Command event) {
                switch (event.commandType) {
                    case cspot::DeviceStateHandler::CommandType::PAUSE:
                        ESP_LOGI(kEspPlayerTag, "Pause command received");
                        this->isPaused = true;
                        this->audioSink->pause();
                        this->notifyPlayerState();
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAY:
                    case cspot::DeviceStateHandler::CommandType::FLUSH:
                    case cspot::DeviceStateHandler::CommandType::SEEK:
                        this->isPaused = false;
                        this->tryStartPlayback();
                        this->notifyPlayerState();
                        break;
                    case cspot::DeviceStateHandler::CommandType::DISC:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        {
                            std::scoped_lock lock(trackMutex);
                            if (tracks.size() > 0) {
                                tracks.at(0)->trackMetrics->endTrack();
                                this->handler->ctx->playbackMetrics->sendEvent(tracks[0]);
                                tracks.clear();
                            }
                        }
                        this->playlistEnd = true;
                        break;
                    case cspot::DeviceStateHandler::CommandType::SKIP_NEXT:
                    case cspot::DeviceStateHandler::CommandType::SKIP_PREV:
                        this->notifyTrackTransition(true);
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        lastTrackHash = 0;
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAYBACK_START:
                        cspot::PlayPathProfiler::mark("esp.playback_start");
                        logFirstAudioPending_ = true;
                        this->notifyTrackTransition(true);
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        lastTrackHash = 0;
                        {
                            std::scoped_lock lock(trackMutex);
                            if (tracks.size()) {
                                tracks.clear();
                            }
                        }
                        this->isPaused = false;
                        this->playlistEnd = false;
                        this->notifyPlayerState();
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAYBACK: {
                        cspot::PlayPathProfiler::mark("esp.playback_track_received");
                        std::scoped_lock lock(trackMutex);
                        tracks.push_back(
                                std::get<std::shared_ptr<cspot::QueuedTrack>>(event.data));
                    }
                        this->isPaused = false;
                        this->playlistEnd = false;
                        this->notifyPlayerState();
                        break;
                    case cspot::DeviceStateHandler::CommandType::DEPLETED:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        this->playlistEnd = true;
                        this->notifyPlayerState();
                        break;
                    case cspot::DeviceStateHandler::CommandType::VOLUME: {
                        int volume = std::get<int32_t>(event.data);
                        (void) volume;
                        break;
                    }
                    default:
                        break;
                }
            };
    startTask();
}

EspPlayer::~EspPlayer() {
    disconnect();
}

void EspPlayer::resetPrebuffer() {
    isPreBuffered = false;
    needsInitialPrebuffer_ = true;
}

void EspPlayer::setTrackTransitionCallback(TrackTransitionCallback callback) {
    trackTransitionCallback_ = std::move(callback);
}

void EspPlayer::notifyTrackTransition(const bool active) {
    const bool was = trackTransitionActive_.exchange(active, std::memory_order_relaxed);
    if (was == active) {
        return;
    }
    if (trackTransitionCallback_) {
        trackTransitionCallback_(active);
    }
}

void EspPlayer::tryStartPlayback() {
    if (isPaused.load()) {
        return;
    }

    const size_t fillLevel = this->audioSink->getBufferFillLevel();
    const size_t resumeThreshold = needsInitialPrebuffer_
                                   ? kInitialPreBufferBytes
                                   : kRecoveryPreBufferBytes;
    if (!this->audioSink->isPlaying()) {
        if (fillLevel < resumeThreshold) {
            isPreBuffered = false;
            return;
        }
        {
            char detail[48];
            snprintf(detail, sizeof(detail), "fill=%u threshold=%u",
                     (unsigned) fillLevel, (unsigned) resumeThreshold);
            cspot::PlayPathProfiler::mark("esp.resume_pipeline", detail);
            ESP_LOGI(kEspPlayerTag,
                     "Spotify prebuffer ready: fill=%u threshold=%u (%s)",
                     (unsigned) fillLevel,
                     (unsigned) resumeThreshold,
                     needsInitialPrebuffer_ ? "initial" : "recovery");
            this->audioSink->resume();
            this->notifyTrackTransition(false);
            this->notifyPlayerState();
        }
        isPreBuffered = true;
        needsInitialPrebuffer_ = false;
        return;
    }

    if (fillLevel >= kInitialPreBufferBytes) {
        isPreBuffered = true;
        needsInitialPrebuffer_ = false;
    }
}

void EspPlayer::monitorPlaybackBuffer() {
    if (isPaused.load() || playlistEnd.load() || !this->audioSink->isPlaying()) {
        return;
    }

    const size_t fillLevel = this->audioSink->getBufferFillLevel();
    if (fillLevel > kPauseLowWaterBytes) {
        return;
    }

    isPreBuffered = false;

    if (btspeaker::diagnostics_enabled) {
        cspot::flushDecodeDiagnosticsBeforeStall();
        cspot::logPlaybackStallVerdict(kEspPlayerTag,
                                fillLevel,
                                this->audioSink->getBufferCapacity(),
                                true,
                                static_cast<uint32_t>(kPauseLowWaterBytes),
                                static_cast<uint32_t>(kRecoveryPreBufferBytes),
                                "buffer_low_pause");
    }

    this->audioSink->pause();
    ESP_LOGW(kEspPlayerTag,
             "Spotify buffer low: fill=%u threshold=%u; pausing until prebuffer=%u",
             (unsigned) fillLevel,
             (unsigned) kPauseLowWaterBytes,
             (unsigned) kRecoveryPreBufferBytes);
}

void EspPlayer::notifyPlayerState() {
    if (!handler->is_active) {
        return;
    }
    handler->putPlayerState(PutStateReason::PutStateReason_PLAYER_STATE_CHANGED,
                            false, true);
}

void EspPlayer::sendPlayerStateHeartbeat() {
    if (!handler->is_active || !handler->trackQueue) {
        return;
    }
    if (!audioSink->isPlaying() && isPaused.load()) {
        return;
    }
    handler->putPlayerState(PutStateReason::PutStateReason_PLAYER_STATE_CHANGED,
                            false, false);
}

void EspPlayer::anchorTrackPlayback(size_t trackId) {
    if (!handler->trackQueue || handler->trackQueue->preloadedTracks.empty()) {
        return;
    }

    const auto &head = handler->trackQueue->preloadedTracks[0];
    head->trackMetrics->startTrackPlaying(head->requestedPosition);
    ESP_LOGD(kEspPlayerTag, "Anchored playback for track %zu at pos %llu",
             trackId, (unsigned long long) head->requestedPosition);

    lastTrackHash = trackId;
    notifyPlayerState();
}

void EspPlayer::recordFeedDiagnostics(size_t requested,
                                      size_t written,
                                      uint32_t feedUs,
                                      size_t fillBefore,
                                      size_t fillAfter) {
#ifdef ESP_PLATFORM
    if (!btspeaker::diagnostics_enabled) {
        return;
    }

    const int64_t nowUs = esp_timer_get_time();
    if (feedDiagWindowStartUs_ == 0) {
        feedDiagWindowStartUs_ = nowUs;
    }
    if (feedDiagLastCallUs_ != 0) {
        const uint32_t gapMs = static_cast<uint32_t>((nowUs - feedDiagLastCallUs_) / 1000);
        if (gapMs > feedDiagMaxGapMs_) {
            feedDiagMaxGapMs_ = gapMs;
        }
    }
    feedDiagLastCallUs_ = nowUs;

    feedDiagCalls_++;
    feedDiagRequestedBytes_ += requested;
    feedDiagWrittenBytes_ += written;
    feedDiagSumFeedUs_ += feedUs;
    if (feedUs > feedDiagMaxFeedUs_) {
        feedDiagMaxFeedUs_ = feedUs;
    }
    if (written == 0) {
        feedDiagZeroWrites_++;
    } else if (written < requested) {
        feedDiagShortWrites_++;
    }
    if (fillBefore < feedDiagMinFillBefore_) {
        feedDiagMinFillBefore_ = fillBefore;
    }
    if (fillAfter < feedDiagMinFillAfter_) {
        feedDiagMinFillAfter_ = fillAfter;
    }
    if (fillAfter > feedDiagMaxFillAfter_) {
        feedDiagMaxFillAfter_ = fillAfter;
    }

    if ((nowUs - feedDiagWindowStartUs_) < kSpotifyFeedDiagIntervalUs) {
        return;
    }

    const uint32_t avgFeedUs = feedDiagCalls_ > 0
                               ? static_cast<uint32_t>(feedDiagSumFeedUs_ / feedDiagCalls_)
                               : 0;
    const uint32_t writtenRate = static_cast<uint32_t>(
            (feedDiagWrittenBytes_ * 1000000ULL) /
            static_cast<uint64_t>(nowUs - feedDiagWindowStartUs_));
    ESP_LOGI(kEspPlayerTag,
             "[SPOTIFY_FEED_DIAG] calls=%lu req=%lu written=%lu rate=%luB/s zero=%lu short=%lu "
             "gap_max_ms=%lu feed_us(avg=%lu max=%lu) fill_before_min=%u fill_after=%u..%u playing=%d",
             static_cast<unsigned long>(feedDiagCalls_),
             static_cast<unsigned long>(feedDiagRequestedBytes_),
             static_cast<unsigned long>(feedDiagWrittenBytes_),
             static_cast<unsigned long>(writtenRate),
             static_cast<unsigned long>(feedDiagZeroWrites_),
             static_cast<unsigned long>(feedDiagShortWrites_),
             static_cast<unsigned long>(feedDiagMaxGapMs_),
             static_cast<unsigned long>(avgFeedUs),
             static_cast<unsigned long>(feedDiagMaxFeedUs_),
             static_cast<unsigned>(feedDiagMinFillBefore_ == SIZE_MAX ? 0 : feedDiagMinFillBefore_),
             static_cast<unsigned>(feedDiagMinFillAfter_ == SIZE_MAX ? 0 : feedDiagMinFillAfter_),
             static_cast<unsigned>(feedDiagMaxFillAfter_),
             this->audioSink->isPlaying() ? 1 : 0);

    cspot::PlaybackFeedSnapshot feed_snapshot{};
    feed_snapshot.feed_rate_bps = writtenRate;
    cspot::publishFeedSnapshot(feed_snapshot);

    feedDiagWindowStartUs_ = nowUs;
    feedDiagCalls_ = 0;
    feedDiagShortWrites_ = 0;
    feedDiagZeroWrites_ = 0;
    feedDiagMaxGapMs_ = 0;
    feedDiagMaxFeedUs_ = 0;
    feedDiagRequestedBytes_ = 0;
    feedDiagWrittenBytes_ = 0;
    feedDiagSumFeedUs_ = 0;
    feedDiagMinFillBefore_ = SIZE_MAX;
    feedDiagMinFillAfter_ = SIZE_MAX;
    feedDiagMaxFillAfter_ = 0;
#else
    (void) requested;
    (void) written;
    (void) feedUs;
    (void) fillBefore;
    (void) fillAfter;
#endif
}

size_t EspPlayer::feedData(uint8_t *data, size_t len, size_t trackId) {
    if (isPaused.load()) {
        return 0;
    }

    const size_t fillBefore = this->audioSink->getBufferFillLevel();
    const int64_t feedStartUs =
#ifdef ESP_PLATFORM
            esp_timer_get_time();
#else
            0;
#endif
    const size_t written = this->audioSink->feedPCMFrames(data, len);
    const size_t fillAfter = this->audioSink->getBufferFillLevel();
    const uint32_t feedUs =
#ifdef ESP_PLATFORM
            static_cast<uint32_t>(esp_timer_get_time() - feedStartUs);
#else
            0;
#endif
    recordFeedDiagnostics(len, written, feedUs, fillBefore, fillAfter);
    if (logFirstAudioPending_ && written > 0 && cspot::PlayPathProfiler::sessionActive()) {
        logFirstAudioPending_ = false;
        char detail[48];
        snprintf(detail, sizeof(detail), "bytes=%u fill=%u",
                 (unsigned) written,
                 (unsigned) this->audioSink->getBufferFillLevel());
        cspot::PlayPathProfiler::mark("esp.first_audio_written", detail);
        cspot::notifyPlayFirstAudio();
    }

    if (this->audioSink->getBufferFillLevel() >= kInitialPreBufferBytes) {
        isPreBuffered = true;
        needsInitialPrebuffer_ = false;
    }

    if (trackId != lastTrackHash) {
        if (lastTrackHash != 0) {
            pendingTrackEnd = true;
        }
        anchorTrackPlayback(trackId);
    }

    tryStartPlayback();
    return written;
}

void EspPlayer::processPendingTrackEnd() {
    if (!pendingTrackEnd.exchange(false)) {
        return;
    }

    std::scoped_lock lock(trackMutex);
    if (tracks.size() > 0) {
        tracks.at(0)->trackMetrics->endTrack();
        this->handler->ctx->playbackMetrics->sendEvent(tracks[0]);
        CSPOT_LOG(info, "TRACK ENDED (deferred)");

        tracks.pop_front();
        if (this->playlistEnd) {
            tracks.clear();
        }
    }
    this->handler->trackPlayer->onTrackEnd(true);
}

void EspPlayer::runTask() {
    ESP_LOGI(kEspPlayerTag, "Starting EspPlayer task");

#ifdef ESP_PLATFORM
    const TickType_t pollPeriod = pdMS_TO_TICKS(100);
    const TickType_t heartbeatPeriod = pdMS_TO_TICKS(kPositionSyncIntervalMs);
    TickType_t nextHeartbeat = xTaskGetTickCount() + heartbeatPeriod;
#endif
    uint32_t logCounter = 0;

    while (isRunning) {
        processPendingTrackEnd();
        monitorPlaybackBuffer();

#ifdef ESP_PLATFORM
        const TickType_t now = xTaskGetTickCount();
        if (now >= nextHeartbeat) {
            sendPlayerStateHeartbeat();
            nextHeartbeat += heartbeatPeriod;
            if ((int32_t) (now - nextHeartbeat) >= (int32_t) heartbeatPeriod) {
                nextHeartbeat = now + heartbeatPeriod;
            }
        }
        vTaskDelay(pollPeriod);
#else
        sendPlayerStateHeartbeat();
        BELL_SLEEP_MS(kPositionSyncIntervalMs);
#endif

        if ((logCounter++ % 50) == 0) {
            ESP_LOGD(kEspPlayerTag, "Buffer size: %u bytes",
                     (unsigned) this->audioSink->getBufferFillLevel());
        }
    }

    ESP_LOGI(kEspPlayerTag, "EspPlayer task exiting");
}

void EspPlayer::disconnect() {
    ESP_LOGI(kEspPlayerTag, "EspPlayer disconnecting");
    isRunning = false;
    notifyTrackTransition(false);
    std::scoped_lock lock(trackMutex);
    tracks.clear();
}
