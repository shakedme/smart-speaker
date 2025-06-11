#pragma once

#include <memory>
#include <string>
#include <atomic>
#include "esp_raii_wrapper.hpp"
#include "DeviceStateHandler.h"

namespace cspot {
    struct Context;
    class LoginBlob;
    class DeviceStateHandler;
}

namespace bell {
    class Task;
    class WrappedSemaphore;
    class CircularBuffer;
}

namespace btspeaker {

class SpotifyPipeline {
public:
    SpotifyPipeline();
    ~SpotifyPipeline();

    // Delete copy operations
    SpotifyPipeline(const SpotifyPipeline&) = delete;
    SpotifyPipeline& operator=(const SpotifyPipeline&) = delete;

    // Move operations
    SpotifyPipeline(SpotifyPipeline&&) = default;
    SpotifyPipeline& operator=(SpotifyPipeline&&) = default;

    // Initialization and lifecycle
    esp_err_t initialize();
    esp_err_t start();
    esp_err_t stop();
    esp_err_t pause();
    esp_err_t resume();

    // Playback control
    esp_err_t play(const std::string& uri);
    esp_err_t search(const std::string& query);
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);
    bool isPlaying() const { return is_playing_; }

    // Audio data handling
    void feedAudioData(uint8_t* data, size_t len);

private:
    std::shared_ptr<cspot::Context> context_;
    std::shared_ptr<cspot::LoginBlob> login_blob_;
    std::shared_ptr<cspot::DeviceStateHandler> device_handler_;
    std::shared_ptr<bell::WrappedSemaphore> login_semaphore_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_initialized_;
    std::atomic<bool> is_running_;
    uint8_t volume_;

    // Internal helpers
    void handleSpotifyCommand(const cspot::DeviceStateHandler::Command& command);
    bool isWifiConnected();
};

} // namespace btspeaker 