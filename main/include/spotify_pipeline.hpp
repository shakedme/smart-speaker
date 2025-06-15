#pragma once

#include <memory>
#include <string>
#include <atomic>
#include "esp_raii_wrapper.hpp"
#include "DeviceStateHandler.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "../include/spotify_config.hpp"

// cspot includes
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "DeviceStateHandler.h"
#include "../../cspot/esp32/main/EspPlayer.h"
#include "PCM5102AudioSink.h"

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
    esp_err_t playURI(const std::string& uri);
    esp_err_t search(const std::string& query);
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);

    // Authentication methods
    esp_err_t startZeroconfDiscovery(); // For Spotify Connect pairing
    esp_err_t loadStoredAuthentication();
    esp_err_t saveAuthentication();
    void clearAuthentication();

    // Status
    bool isAuthenticated() const { return is_authenticated_; }
    bool isRunning() const { return isRunning_; }
    bool isPlaying() const { return isPlaying_; }

    // Callback for successful authentication
    esp_err_t onSpotifyConnected(const std::shared_ptr<cspot::LoginBlob>& blob);

    static constexpr const char* TAG = "SpotifyPipeline";
    static constexpr const char* NVS_NAMESPACE = "spotify_auth";
    static constexpr const char* NVS_AUTH_KEY = "auth_blob";

private:
    std::shared_ptr<cspot::Context> context_;
    std::shared_ptr<cspot::LoginBlob> loginBlob_;
    std::shared_ptr<cspot::DeviceStateHandler> deviceHandler_;
    std::shared_ptr<bell::WrappedSemaphore> loginSemaphore_;
    std::atomic<bool> isPlaying_;
    std::atomic<bool> isInitialized_;
    std::atomic<bool> isRunning_;
    std::atomic<uint8_t> volume_;

    // Authentication state
    std::atomic<bool> is_authenticated_;
    std::string stored_username_;

    // Audio streaming components
    std::shared_ptr<PCM5102AudioSink> audioSink_;
    std::unique_ptr<EspPlayer> espPlayer_;

    // HTTP server for Zeroconf authentication
    httpd_handle_t http_server_;

    // Private methods
    esp_err_t initializeAudioComponents();
    esp_err_t startAudioStreaming();
    esp_err_t stopAudioStreaming();
    esp_err_t startAuthentication();
    esp_err_t loadStoredAuth();
    esp_err_t saveAuthBlob();
    void clearStoredAuth();
    esp_err_t startHttpServer();
    void stopHttpServer();
    void handleSpotifyCommand(const cspot::DeviceStateHandler::Command& command);

    // HTTP handlers
    static esp_err_t spotify_info_get_handler(httpd_req_t *req);
    static esp_err_t spotify_info_post_handler(httpd_req_t *req);
    
    // Helper methods
    static SpotifyPipeline* getInstanceFromRequest(httpd_req_t* req);
};

} // namespace btspeaker 