#include "../include/spotify_pipeline.hpp"
#include "../include/spotify_config.hpp"
#include "../include/adf_audio_sink.hpp"
#include "include/spotify_auth_manager.hpp"

#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
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

    esp_err_t SpotifyPipeline::start() {
        if (!authManager->isAuthenticated()) {
            ESP_LOGE(TAG, "Not authenticated - connect via Spotify app first.");
            return ESP_ERR_INVALID_STATE;
        }
        if (isRunning_) {
            ESP_LOGW(TAG, "Already running");
            return ESP_OK;
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
                3,
                taskStack,
                taskBuffer,
                0
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
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::resume() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        if (isPlaying) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Resuming Spotify playback");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
            isPlaying = true;
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::playURI(const std::string &uri) {
        ESP_LOGI(TAG, "[play] Playing Spotify URI: %s", uri.c_str());
        
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
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::SKIP_NEXT);
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::skipPrevious() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "Skipping to previous track");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::SKIP_PREV);
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

                if (!pipeline->nextUri.empty()) {
                    pipeline->playURI(pipeline->nextUri);
                }

                while (pipeline->isRunning_) {
                    if (pipeline->context && pipeline->context->session) {
                        try {
                            pipeline->context->session->handlePacket();
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

            // Add detailed debugging of the blob being used for authentication
            ESP_LOGI(TAG, "🔧 DEBUG: Auth blob details before authentication:");
            ESP_LOGI(TAG, "  - Username: %s", loginBlob->getUserName().c_str());
            ESP_LOGI(TAG, "  - Device ID: %s", loginBlob->getDeviceId().c_str());
            ESP_LOGI(TAG, "  - Device Name: %s", loginBlob->getDeviceName().c_str());
            ESP_LOGI(TAG, "  - Auth Type: %d", loginBlob->authType);
            ESP_LOGI(TAG, "  - Auth Data Size: %d bytes", loginBlob->authData.size());

            if (loginBlob->authData.size() > 0) {
                ESP_LOGI(TAG, "  - First 8 bytes of auth data:");
                for (int i = 0; i < std::min(8, (int) loginBlob->authData.size()); i++) {
                    ESP_LOGI(TAG, "    [%d]: 0x%02X", i, loginBlob->authData[i]);
                }
            }

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

                    ESP_LOGI(TAG, "Authenticating with login blob");
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
            audioSink->start();
            ESP_LOGI(TAG, "Creating ESP player");
            espPlayer = std::make_unique<EspPlayer>(audioSink, deviceStateHandler);
            ESP_LOGI(TAG, "Starting session task");
            context->session->startTask();
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


} // namespace btspeaker