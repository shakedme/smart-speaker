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

    // Authentication methods
    esp_err_t authenticateWithCredentials(const std::string& username, const std::string& password);
    esp_err_t startZeroconfDiscovery(); // For Spotify Connect pairing
    esp_err_t loadStoredAuth(); // Load from NVS storage
    esp_err_t saveAuthBlob(); // Save current auth to NVS storage
    void clearStoredAuth(); // Clear stored authentication

    // Authentication status
    bool isAuthenticated() const { return is_authenticated_; }
    const std::string& getStoredUsername() const { return stored_username_; }

    // Connection callback for Zeroconf discovery
    esp_err_t onSpotifyConnected(std::shared_ptr<cspot::LoginBlob> blob);

    // Audio data handling
    void feedAudioData(uint8_t* data, size_t len);

    static constexpr const char* TAG = "SpotifyPipeline";
    static constexpr const char* NVS_NAMESPACE = "spotify_auth";
    static constexpr const char* NVS_AUTH_KEY = "auth_blob";

private:
    std::shared_ptr<cspot::Context> context_;
    std::shared_ptr<cspot::LoginBlob> login_blob_;
    std::shared_ptr<cspot::DeviceStateHandler> device_handler_;
    std::shared_ptr<bell::WrappedSemaphore> login_semaphore_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_initialized_;
    std::atomic<bool> is_running_;
    uint8_t volume_;

    // Authentication state
    std::atomic<bool> is_authenticated_;
    std::string stored_username_;

    // Internal helpers
    void handleSpotifyCommand(const cspot::DeviceStateHandler::Command& command);
    bool isWifiConnected();
    esp_err_t playUriInternal(const std::string& uri);
    
    // Authentication helpers
    esp_err_t startAuthentication();
    esp_err_t handleZeroconfPairing();
    esp_err_t loadAuthFromNVS();
    esp_err_t saveAuthToNVS();
};

} // namespace btspeaker 