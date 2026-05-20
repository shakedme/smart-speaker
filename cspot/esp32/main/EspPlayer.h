#pragma once

#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint8_t
#include <atomic>    // for atomic
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex
#include <string>    // for string

#include "AudioSink.h"           // for AudioSink
#include "BellTask.h"            // for Task
#include "DeviceStateHandler.h"  // for DeviceStateHandler

namespace cspot {
    class DeviceStateHandler;
}  // namespace cspot

class EspPlayer : public bell::Task {
public:
    static constexpr size_t kPreBufferBytes = 192 * 1024;
    static constexpr size_t kPlayStartPreBufferBytes = 128 * 1024;

    EspPlayer(std::shared_ptr<AudioSink> sink,
              std::shared_ptr<cspot::DeviceStateHandler> handler);

    ~EspPlayer();

    void disconnect();

private:
    static constexpr uint32_t kPositionSyncIntervalMs = 1000;

    void runTask() override;
    size_t feedData(uint8_t *data, size_t len, size_t trackId);
    void tryStartPlayback();
    void anchorTrackPlayback(size_t trackId);
    void processPendingTrackEnd();
    void notifyPlayerState();
    void sendPlayerStateHeartbeat();

    std::shared_ptr<cspot::DeviceStateHandler> handler;
    std::shared_ptr<AudioSink> audioSink;
    std::deque<std::shared_ptr<cspot::QueuedTrack>> tracks = {};

    std::atomic<bool> isPaused = false;
    std::atomic<bool> playlistEnd = false;
    std::atomic<bool> isRunning = true;
    std::atomic<bool> isPreBuffered = false;
    std::atomic<bool> pendingTrackEnd = false;
    bool logFirstAudioPending_ = false;
    bool reducedPrebufferActive_ = false;
    size_t lastTrackHash = 0;

    std::mutex trackMutex;
};
