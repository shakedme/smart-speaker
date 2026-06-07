#include "../include/spotify_pipeline.hpp"
#include "../include/task_affinity.hpp"
#include "../include/spotify_config.hpp"
#include "../include/adf_audio_sink.hpp"
#include "include/spotify_auth_manager.hpp"

#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <algorithm>
#include <sstream>
#include <string>
#include <cstring>
#include <random>
#include <stdexcept>

// cspot includes  
#include "Utils.h"
#include "BellUtils.h"
#include "Logger.h"
#include "CSpotContext.h"
#include "MercurySession.h"
#include "Session.h"
#include "DeviceStateHandler.h"
#include "LoginBlob.h"
#include "WrappedSemaphore.h"
#include "../../cspot/esp32/main/EspPlayer.h"
#include "include/memory_utils.hpp"

static const char *TAG = "SpotifyPipeline";

namespace btspeaker {
    namespace {
        constexpr int kSpotifyConnectMaxAttempts = 5;
        constexpr uint32_t kSpotifyConnectBaseBackoffMs = 500;
        constexpr uint32_t kSpotifyConnectMaxBackoffMs = 8000;

        uint32_t spotifyConnectBackoffMs(int attempt) {
            uint32_t delay = kSpotifyConnectBaseBackoffMs;
            for (int i = 1; i < attempt; i++) {
                delay *= 2;
                if (delay >= kSpotifyConnectMaxBackoffMs) {
                    return kSpotifyConnectMaxBackoffMs;
                }
            }
            return delay;
        }
    }

    SpotifyPipeline::SpotifyPipeline()
            : isPlaying(false),
              isInitialized(false),
              isRunning_(false),
              volume(100) {
        ESP_LOGI(TAG, "Initializing Spotify pipeline");

        audioSink = std::make_shared<ADFAudioSink>();
        authManager = std::make_unique<SpotifyAuthManager>();
        authManager->loadStoredAuth();
        authManager->startZeroconfDiscovery();
        isInitialized = true;
        ESP_LOGI(TAG, "Spotify pipeline initialized successfully");
    }

    SpotifyPipeline::~SpotifyPipeline() {
        stop();
        ESP_LOGI(TAG, "SpotifyPipeline destroyed");
    }

    esp_err_t SpotifyPipeline::ensureAudioSinkReady() {
        if (!audioSink) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!audioSink->isInitialized()) {
            audioSink->prepare();
            audioSink->start();
            audioSink->pause();
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::playPendingURIIfReady() {
        if (nextUri.empty() || !deviceStateHandler) {
            return ESP_OK;
        }

        const std::string uri = nextUri;
        nextUri.clear();
        return playURI(uri);
    }

    esp_err_t SpotifyPipeline::start() {
        if (!authManager->isAuthenticated()) {
            ESP_LOGE(TAG, "Not authenticated - connect via Spotify app first.");
            return ESP_ERR_INVALID_STATE;
        }
        if (isRunning_) {
            ESP_LOGW(TAG, "Already running");
            esp_err_t ret = ensureAudioSinkReady();
            if (ret != ESP_OK) {
                return ret;
            }
            return playPendingURIIfReady();
        }

        const size_t stackSizeWords = 8192 * 4;
        taskStack = (StackType_t *) heap_caps_malloc(
                stackSizeWords * sizeof(StackType_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        taskBuffer = (StaticTask_t *) heap_caps_malloc(
                sizeof(StaticTask_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (!taskStack || !taskBuffer) {
            ESP_LOGE(TAG, "Failed to allocate memory for Spotify task");
            if (taskStack) {
                heap_caps_free(taskStack);
                taskStack = nullptr;
            }
            if (taskBuffer) {
                heap_caps_free(taskBuffer);
                taskBuffer = nullptr;
            }
            return ESP_FAIL;
        }
        packetHandlerTaskHandle = xTaskCreateStaticPinnedToCore(
                spotifyMainTask,
                "spotify_main",
                stackSizeWords,
                this,
                kSpotifyMainPriority,
                taskStack,
                taskBuffer,
                kSpotifyMainCore
        );
        if (packetHandlerTaskHandle == nullptr) {
            ESP_LOGE(TAG, "Failed to create Spotify main task");
            heap_caps_free(taskStack);
            heap_caps_free(taskBuffer);
            taskStack = nullptr;
            taskBuffer = nullptr;
            return ESP_FAIL;
        }
        isRunning_ = true;
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::stop() {
        if (!isInitialized || !isRunning_) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Stopping Spotify pipeline");
        isRunning_ = false;
        isPlaying = false;
        if (packetHandlerTaskHandle != nullptr) {
            vTaskDelete(packetHandlerTaskHandle);
            packetHandlerTaskHandle = nullptr;
        }
        if (taskStack) {
            heap_caps_free(taskStack);
            taskStack = nullptr;
        }
        if (taskBuffer) {
            heap_caps_free(taskBuffer);
            taskBuffer = nullptr;
        }
        audioSink->pause();
        if (deviceStateHandler) {
            deviceStateHandler->disconnect();
        }
        if (espPlayer) {
            espPlayer->disconnect();
        }
        audioSink->cleanupPipeline();
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::pause() {
        ESP_LOGI(TAG, "Pausing Spotify playback");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PAUSE);
        }
        audioSink->pause();
        isPlaying = false;
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::resume() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!isPlaybackPaused()) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Resuming Spotify playback");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
            isPlaying = true;
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::suspendForVoiceResponse() {
        if (!isInitialized || !isRunning_) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Suspending Spotify for voice response");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PAUSE);
        }
        isPlaying = false;
        audioSink->pause();
        if (audioSink->isInitialized()) {
            audioSink->cleanupPipeline();
        }
        if (espPlayer) {
            espPlayer->resetPrebuffer();
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::resumeAfterVoiceResponse() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!isRunning_) {
            return start();
        }

        ESP_LOGI(TAG, "Resuming Spotify after voice response");
        const esp_err_t ret = ensureAudioSinkReady();
        if (ret != ESP_OK) {
            return ret;
        }
        return resume();
    }

    esp_err_t SpotifyPipeline::playURI(const std::string &uri) {
        ESP_LOGI(TAG, "[play] Playing Spotify URI: %s", uri.c_str());

        if (!deviceStateHandler || !deviceStateHandler->trackPlayer || !deviceStateHandler->playerContext ||
            !deviceStateHandler->ctx) {
            ESP_LOGW(TAG, "Spotify session is not ready; queueing URI");
            setNextURI(uri);
            return ESP_ERR_INVALID_STATE;
        }

        const esp_err_t ret = ensureAudioSinkReady();
        if (ret != ESP_OK) {
            return ret;
        }
        
        if (deviceStateHandler->trackPlayer) {
            deviceStateHandler->trackPlayer->stop();
        }
        deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::DEPLETED);
        
        // Clear current state
        deviceStateHandler->currentTracks.clear();
        deviceStateHandler->offset = 0;
        if (deviceStateHandler->trackQueue) {
            deviceStateHandler->trackQueue->preloadedTracks.clear();
        }
        
        // Set up proper context for the track
        if (deviceStateHandler->playerContext->next_page_url != NULL) {
            unreference(&(deviceStateHandler->playerContext->next_page_url));
        }
        deviceStateHandler->playerContext->radio_offset = 0;
        
        // Create the track with proper metadata
        ProvidedTrack track = ProvidedTrack_init_zero;
        track.uri = strdup(uri.c_str());
        track.provider = strdup("context"); // Use "context" instead of "queue"
        track.original_index = 0;
        
        // Set up context URI based on the track URI for autoplay/recommendations
        std::string contextUri = uri;
        if (uri.find("spotify:track:") == 0) {// This should enable autoplay/radio mode
            contextUri = uri;
        }
        
        // Clean up existing context
        unreference(&(deviceStateHandler->device.player_state.context_uri));
        unreference(&(deviceStateHandler->device.player_state.context_url));
        
        // Set context URI to enable track continuation
        deviceStateHandler->device.player_state.context_uri = strdup(contextUri.c_str());
        deviceStateHandler->device.player_state.context_url = strdup(contextUri.c_str());
        
        // Add the track to current tracks
        deviceStateHandler->currentTracks.push_back(track);
        
        // Set up player state
        deviceStateHandler->device.player_state.is_playing = true;
        deviceStateHandler->device.player_state.has_is_playing = true;
        deviceStateHandler->device.player_state.has_track = true;
        deviceStateHandler->device.player_state.track = track;
        deviceStateHandler->device.player_state.timestamp = 
            deviceStateHandler->ctx->timeProvider->getSyncedTimestamp();
        
        // Generate session and playback IDs for proper state tracking
        std::vector<uint8_t> random_bytes;
        static std::uniform_int_distribution<int> d(0, 255);
        for (int i = 0; i < 16; i++) {
            random_bytes.push_back(d(deviceStateHandler->ctx->rng));
        }
        
        unreference(&(deviceStateHandler->device.player_state.session_id));
        deviceStateHandler->device.player_state.session_id =
            strdup(bytesToHexString(random_bytes).c_str());

        unreference(&(deviceStateHandler->device.player_state.playback_id));
        random_bytes.clear();
        for (int i = 0; i < 16; i++) {
            random_bytes.push_back(d(deviceStateHandler->ctx->rng));
        }
        deviceStateHandler->device.player_state.playback_id = 
            strdup(base64Encode(random_bytes).c_str());
        
        // Set active state
        if (!deviceStateHandler->is_active) {
            deviceStateHandler->started_playing_at = 
                deviceStateHandler->ctx->timeProvider->getSyncedTimestamp();
            deviceStateHandler->is_active = true;
        }
        
        // Trigger track loading and autoplay setup
        deviceStateHandler->reloadPreloadedTracks = true;
        std::vector<std::pair<std::string, std::string>> metadata_map;
        
        // Enable autoplay by setting appropriate metadata
        metadata_map.push_back(std::make_pair("autoplay", "true"));
        
        deviceStateHandler->playerContext->resolveTracklist(metadata_map,
                                                            deviceStateHandler->reloadTrackList,
                                                            true, true);
        
        ESP_LOGI(TAG, "Successfully initiated playback for URI: %s with autoplay enabled", uri.c_str());
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::skipNext() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Skipping to next track");
        if (deviceStateHandler) {
            deviceStateHandler->requestSkipNext();
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::skipPrevious() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Skipping to previous track");
        if (deviceStateHandler) {
            deviceStateHandler->requestSkipPrevious();
        }
        return ESP_OK;
    }

    void SpotifyPipeline::setVolume(uint8_t newVolume) {
        volume = newVolume;
        ESP_LOGI(TAG, "Setting volume to: %d", volume.load());
        if (audioSink) {
            // PCM5102AudioSink doesn't have volume control, so we just store it
        }
    }

    void SpotifyPipeline::spotifyMainTask(void *parameter) {
        auto *pipeline = static_cast<SpotifyPipeline *>(parameter);
        if (pipeline->initCSpotComponents() == ESP_OK) {
            try {
                ESP_LOGI(TAG, "Starting Spotify session");

                (void) pipeline->playPendingURIIfReady();

                while (pipeline->isRunning_) {
                    if (pipeline->context && pipeline->context->session) {
                        try {
                            pipeline->context->session->handlePacket();
                            if (pipeline->deviceStateHandler) {
                                pipeline->deviceStateHandler->tryFlushDeferredPlayerState();
                                pipeline->deviceStateHandler->tryProcessDeferredContextResolve();
                                pipeline->deviceStateHandler->tryCompleteDeferredReload();
                            }
                        } catch (const std::exception &e) {
                            ESP_LOGE(TAG, "Exception processing packet: %s", e.what());
                            std::string error_msg = e.what();
                            if (error_msg.find("Connection failed") != std::string::npos ||
                                error_msg.find("SSL handshake failed") != std::string::npos ||
                                error_msg.find("network") != std::string::npos) {
                                ESP_LOGE(TAG, "Critical network error detected, restarting session...");
                                vTaskDelay(pdMS_TO_TICKS(5000));
                            } else {
                                ESP_LOGE(TAG, "Non-critical error, continuing...");
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }
                        } catch (...) {
                            ESP_LOGE(TAG, "Unknown exception processing packet");
                            ESP_LOGE(TAG, "Continuing with next packet...");
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    } else {
                        ESP_LOGW(TAG, "Context or session not available, waiting...");
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "Exception in Spotify main task: %s", e.what());
            } catch (...) {
                ESP_LOGE(TAG, "Unknown exception in Spotify main task");
            }
        } else {
            pipeline->failureCallback();
        }

        ESP_LOGI(TAG, "Spotify main task exiting");
        pipeline->isRunning_ = false;
        vTaskDelete(nullptr);
    }

    void SpotifyPipeline::setAuthenticatedCallback(AuthenticatedCallback cb) {
        authManager->setAuthenticatedCallback(std::move(cb));
    }

    esp_err_t SpotifyPipeline::initCSpotComponents() {
        try {
            ESP_LOGI(TAG, "Creating Spotify context");
            auto loginBlob = authManager->getLoginBlob();
            for (int attempt = 1; attempt <= kSpotifyConnectMaxAttempts; attempt++) {
                try {
                    context = cspot::Context::createFromBlob(loginBlob);
                    if (!context) {
                        ESP_LOGE(TAG, "Failed to create Spotify context from blob");
                        vTaskDelete(nullptr);
                        return ESP_FAIL;
                    }

                    ESP_LOGI(TAG,
                             "Connecting to Spotify access point (attempt %d/%d)",
                             attempt,
                             kSpotifyConnectMaxAttempts);
                    context->session->connectWithRandomAp();

                    context->config.authData = context->session->authenticate(loginBlob);
                    if (context->config.authData.empty()) {
                        throw std::runtime_error("Authentication failed - no auth data received");
                    }

                    break;
                } catch (const std::exception &e) {
                    if (context && context->session) {
                        context->session->close();
                    }
                    context.reset();

                    if (attempt == kSpotifyConnectMaxAttempts) {
                        throw;
                    }

                    const uint32_t delayMs = spotifyConnectBackoffMs(attempt);
                    ESP_LOGW(TAG,
                             "Spotify connect/auth attempt %d/%d failed: %s; retrying in %u ms",
                             attempt,
                             kSpotifyConnectMaxAttempts,
                             e.what(),
                             (unsigned) delayMs);
                    vTaskDelay(pdMS_TO_TICKS(delayMs));
                }
            }
            ESP_LOGI(TAG, "Authentication successful, initializing device state handler and audio sink");
            deviceStateHandler = std::make_shared<cspot::DeviceStateHandler>(context, [this]() {
                ESP_LOGI(TAG, "Spotify session disconnected");
                isRunning_ = false;
            });
            audioSink->prepare();
            espPlayer = std::make_unique<EspPlayer>(audioSink, deviceStateHandler);
            espPlayer->setTrackTransitionCallback([this](const bool active) {
                trackTransitionActive_.store(active, std::memory_order_release);
            });
            audioSink->start();
            audioSink->pause();
            context->session->startTask();
            // Ensure modem sleep stays off while Spotify Connect is active.
            if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
                ESP_LOGI(TAG, "WiFi power save disabled for Spotify session");
            }
            ESP_LOGI(TAG, "CSpot components initialized successfully");
            return ESP_OK;
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Exception initializing CSpot components: %s", e.what());
            if (context) {
                context->session->disconnect();
                context.reset();
            }
            if (deviceStateHandler) {
                deviceStateHandler->disconnect();
                deviceStateHandler.reset();
            }
            if (espPlayer) {
                espPlayer->disconnect();
                espPlayer.reset();
            }
            if (audioSink->isInitialized()) {
                audioSink->cleanupPipeline();
            }
            return ESP_FAIL;
        }

    }

    void SpotifyPipeline::setNextURI(const std::string &uri) {
        nextUri = uri;
    }

    void SpotifyPipeline::setFailureCallback(FailureCallback cb) {
        failureCallback = std::move(cb);
    }

    size_t SpotifyPipeline::getPlaybackBufferFillLevel() const {
        return audioSink ? audioSink->getBufferFillLevel() : 0;
    }

    size_t SpotifyPipeline::getPlaybackBufferCapacity() const {
        return audioSink ? audioSink->getBufferCapacity() : 0;
    }

    bool SpotifyPipeline::isSinkPlaying() const {
        return audioSink && audioSink->isPlaying();
    }

    bool SpotifyPipeline::isRebuffering() const {
        return isRunning_.load(std::memory_order_relaxed) &&
               audioSink &&
               audioSink->isInitialized() &&
               !isPlaybackPaused() &&
               !audioSink->isPlaying();
    }

    bool SpotifyPipeline::isPlaybackPaused() const {
        return espPlayer && espPlayer->isPlaybackPaused();
    }

    bool SpotifyPipeline::isTrackTransitionActive() const {
        return trackTransitionActive_.load(std::memory_order_acquire);
    }

    bool SpotifyPipeline::tryGetCurrentTrackMetadata(TrackMetadata &out) const {
        if (!deviceStateHandler || !deviceStateHandler->trackQueue) {
            return false;
        }

        const auto &trackQueue = deviceStateHandler->trackQueue;
        std::scoped_lock lock(trackQueue->tracksMutex);
        if (trackQueue->preloadedTracks.empty()) {
            return false;
        }

        const auto &track = trackQueue->preloadedTracks.front();
        if (track->trackInfo.name.empty()) {
            return false;
        }

        std::strncpy(out.title, track->trackInfo.name.c_str(), sizeof(out.title) - 1);
        out.title[sizeof(out.title) - 1] = '\0';
        std::strncpy(out.artist, track->trackInfo.artist.c_str(), sizeof(out.artist) - 1);
        out.artist[sizeof(out.artist) - 1] = '\0';
        std::strncpy(out.album, track->trackInfo.album.c_str(), sizeof(out.album) - 1);
        out.album[sizeof(out.album) - 1] = '\0';
        out.duration_ms = track->trackInfo.duration;
        return true;
    }

    const char *SpotifyPipeline::playbackStateName() const {
        if (!isRunning_.load(std::memory_order_acquire)) {
            return "idle";
        }
        if (isPlaybackPaused()) {
            return "paused";
        }
        if (isSinkPlaying()) {
            return "playing";
        }
        return "paused";
    }

} // namespace btspeaker