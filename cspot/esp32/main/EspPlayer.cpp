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
#endif

#include "BellUtils.h"
#include "DeviceStateHandler.h"
#include "Logger.h"
#include "PlayPathProfiler.h"
#include "TrackPlayer.h"

static const char *kEspPlayerTag = "EspPlayer";

EspPlayer::EspPlayer(std::shared_ptr<AudioSink> sink,
                     std::shared_ptr<cspot::DeviceStateHandler> handler)
        // Core 0 + elevated priority for timeline heartbeats; 24KB PSRAM stack
        // is required for putPlayerState/protobuf encoding.
        : bell::Task("esp_player", 24 * 1024, 3, 0) {
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
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        lastTrackHash = 0;
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAYBACK_START:
                        cspot::PlayPathProfiler::mark("esp.playback_start");
                        logFirstAudioPending_ = true;
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
}

void EspPlayer::tryStartPlayback() {
    if (isPaused.load()) {
        return;
    }

    const size_t fillLevel = this->audioSink->getBufferFillLevel();
    if (!this->audioSink->isPlaying()) {
        if (fillLevel < kPreBufferBytes) {
            isPreBuffered = false;
            return;
        }
        {
            char detail[48];
            snprintf(detail, sizeof(detail), "fill=%u threshold=%u",
                     (unsigned) fillLevel, (unsigned) kPreBufferBytes);
            cspot::PlayPathProfiler::mark("esp.resume_pipeline", detail);
            ESP_LOGI(kEspPlayerTag, "Spotify prebuffer ready: fill=%u threshold=%u",
                     (unsigned) fillLevel, (unsigned) kPreBufferBytes);
            this->audioSink->resume();
        }
        isPreBuffered = true;
        return;
    }

    if (fillLevel >= kPreBufferBytes) {
        isPreBuffered = true;
    }
}

void EspPlayer::notifyPlayerState() {
    if (!handler->is_active) {
        return;
    }
    handler->putPlayerState(PutStateReason::PutStateReason_PLAYER_STATE_CHANGED,
                            false, true);
}

void EspPlayer::sendPlayerStateHeartbeat() {
    if (!handler->is_active || !handler->trackQueue ||
        handler->trackQueue->preloadedTracks.empty()) {
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

size_t EspPlayer::feedData(uint8_t *data, size_t len, size_t trackId) {
    if (isPaused.load()) {
        return 0;
    }

    const size_t written = this->audioSink->feedPCMFrames(data, len);
    if (logFirstAudioPending_ && written > 0 && cspot::PlayPathProfiler::sessionActive()) {
        logFirstAudioPending_ = false;
        char detail[48];
        snprintf(detail, sizeof(detail), "bytes=%u fill=%u",
                 (unsigned) written,
                 (unsigned) this->audioSink->getBufferFillLevel());
        cspot::PlayPathProfiler::mark("esp.first_audio_written", detail);
        cspot::notifyPlayFirstAudio();
    }

    if (this->audioSink->getBufferFillLevel() >= kPreBufferBytes) {
        isPreBuffered = true;
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
    std::scoped_lock lock(trackMutex);
    tracks.clear();
}
