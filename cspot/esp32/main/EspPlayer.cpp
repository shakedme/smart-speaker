#include "EspPlayer.h"

#include <cstdint>      // for uint8_t
#include <memory>       // for shared_ptr, make_shared, make_unique
#include <mutex>        // for scoped_lock
#include <utility>      // for move
#include <variant>      // for get
#include <vector>       // for vector
#include <esp_log.h>

#include "BellUtils.h"  // for BELL_SLEEP_MS
#include "DeviceStateHandler.h"  // for SpircHandler, DeviceStateHandler::CommandType
#include "Logger.h"
#include "TrackPlayer.h"  // for TrackPlayer

EspPlayer::EspPlayer(std::shared_ptr<AudioSink> sink,
                     std::shared_ptr<cspot::DeviceStateHandler> handler)
        : bell::Task("esp_player", 24 * 1024, 0, 1) {
    this->handler = std::move(handler);
    this->audioSink = std::move(sink);

    this->handler->trackPlayer->setDataCallback(
            [this](uint8_t *data, size_t bytes, size_t trackId,
                   bool STORAGE_VOLATILE) {
                this->feedData(data, bytes, trackId);
                return bytes;
            });

    this->isPaused = false;

    this->handler->stateCallback =
            [this](cspot::DeviceStateHandler::Command event) {
                switch (event.commandType) {
                    case cspot::DeviceStateHandler::CommandType::PAUSE:
                        this->isPaused = true;
                        this->audioSink->pause();
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAY:
                        this->isPaused = false;
                        this->audioSink->resume();
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
                    case cspot::DeviceStateHandler::CommandType::FLUSH:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        break;
                    case cspot::DeviceStateHandler::CommandType::SEEK:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        break;
                    case cspot::DeviceStateHandler::CommandType::SKIP_NEXT:
                    case cspot::DeviceStateHandler::CommandType::SKIP_PREV:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAYBACK_START:
                        this->audioSink->flush();
                        this->isPreBuffered = false;
                        {
                            std::scoped_lock lock(trackMutex);
                            if (tracks.size())
                                tracks.clear();
                        }
                        this->isPaused = false;
                        this->playlistEnd = false;
                        break;
                    case cspot::DeviceStateHandler::CommandType::PLAYBACK: {
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
                        break;
                    case cspot::DeviceStateHandler::CommandType::VOLUME: {
                        int volume = std::get<int32_t>(event.data);
                        // Handle volume if needed
                        break;
                    }
                    default:
                        break;
                }
            };
    startTask(); // Start the keepalive task
}

void EspPlayer::feedData(uint8_t *data, size_t len, size_t trackId) {
    if (isPaused) {
        BELL_SLEEP_MS(100);
        return;
    }

    if (!this->isPreBuffered && this->audioSink->isPlaying()) {
        audioSink->pause();
    }

    this->audioSink->feedPCMFrames(data, len);

    if (this->audioSink->getBufferFillLevel() > (1024 * 1512) && !this->isPreBuffered) {
        audioSink->resume();
        this->isPreBuffered = true;
    }

    // Track management and metrics
    {
        std::scoped_lock lock(trackMutex);
        this->currentTrackHash = trackId;

        // Handle track changes
        if (lastTrackHash != currentTrackHash && lastTrackHash != 0) {
            if (tracks.size() > 0) {
                tracks.at(0)->trackMetrics->endTrack();
                this->handler->ctx->playbackMetrics->sendEvent(tracks[0]);
                CSPOT_LOG(info, "TRACK ENDED, new track %zu", currentTrackHash);

                tracks.pop_front();
                if (this->playlistEnd) {
                    tracks.clear();
                }
            }
            this->handler->trackPlayer->onTrackEnd(true);
        }

        // Start new track metrics
        if (lastTrackHash != currentTrackHash) {
            if (tracks.size() > 0) {
                tracks.at(0)->trackMetrics->startTrackPlaying(
                        tracks.at(0)->requestedPosition);
            }
            lastTrackHash = currentTrackHash;
        }
    }
}

void EspPlayer::runTask() {
    ESP_LOGI("EspPlayer", "Starting EspPlayer keepalive task");

    while (isRunning) {
        ESP_LOGI("EspPlayer", "Read buffer size: %d", this->audioSink->getBufferFillLevel());
        if (this->audioSink->isPlaying()) {
            this->handler->putPlayerState();
        }
        BELL_SLEEP_MS(1000);
    }

    ESP_LOGI("EspPlayer", "EspPlayer keepalive task exiting");
}

void EspPlayer::disconnect() {
    ESP_LOGI("EspPlayer", "EspPlayer disconnecting");
    isRunning = false;

    // Clean up any remaining tracks
    std::scoped_lock lock(trackMutex);
    tracks.clear();
}
