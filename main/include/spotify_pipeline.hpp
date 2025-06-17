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
    esp_err_t skipNext();
    esp_err_t skipPrevious();
    void setVolume(uint8_t volume);

    // Authentication methods
    esp_err_t startZeroconfDiscovery();

    // Callback for successful authentication
    esp_err_t onSpotifyConnected(const std::shared_ptr<cspot::LoginBlob>& blob);

    static constexpr const char* TAG = "SpotifyPipeline";
    static constexpr const char* NVS_NAMESPACE = "spotify_auth";
    static constexpr const char* NVS_AUTH_KEY = "auth_blob";

private:
    std::shared_ptr<cspot::Context> context;
    std::shared_ptr<cspot::LoginBlob> loginBlob;
    std::shared_ptr<cspot::DeviceStateHandler> deviceStateHandler;
    std::atomic<bool> isPlaying;
    std::atomic<bool> isInitialized;
    std::atomic<bool> isRunning;
    std::atomic<uint8_t> volume;

    // Authentication state
    std::atomic<bool> isAuthenticated;
    std::string storedUsername;

    // Audio streaming components
    std::shared_ptr<PCM5102AudioSink> audioSink;
    std::unique_ptr<EspPlayer> espPlayer;

    // HTTP server for Zeroconf authentication
    httpd_handle_t httpServer;

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

    // HTTP handlers
    static esp_err_t spotifyInfoGetHandler(httpd_req_t *req);
    static esp_err_t spotifyInfoPostHandler(httpd_req_t *req);
    
    // Helper methods
    static SpotifyPipeline* getInstanceFromRequest(httpd_req_t* req);
};

} // namespace btspeaker 