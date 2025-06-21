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
    EspPlayer(std::shared_ptr<AudioSink> sink,
              std::shared_ptr<cspot::DeviceStateHandler> handler);

    void disconnect();

private:
    void runTask() override; // Simple keepalive task
    std::string currentTrackId;
    std::shared_ptr<cspot::DeviceStateHandler> handler;
    std::shared_ptr<AudioSink> audioSink;
    std::deque<std::shared_ptr<cspot::QueuedTrack>> tracks = {};

    void feedData(uint8_t *data, size_t len, size_t trackId);

    std::atomic<bool> isPaused = false;
    std::atomic<bool> playlistEnd = false;
    std::atomic<bool> isRunning = true;
    std::atomic<bool> isPreBuffered = false;
    size_t currentTrackHash = 0;
    size_t lastTrackHash = 0;
    
    // Track state management
    std::mutex trackMutex;
};
