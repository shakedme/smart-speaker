#include "../include/spotify_pipeline.hpp"
#include "../include/spotify_config.hpp"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <algorithm>

// cspot includes  
#include "Utils.h"
#include "BellUtils.h"
#include "Logger.h"
#include "CSpotContext.h"
#include "DeviceStateHandler.h"
#include "LoginBlob.h"
#include "WrappedSemaphore.h"
#include "PCM5102AudioSink.h"
#include "../../cspot/esp32/main/EspPlayer.h"

static const char *TAG = "SpotifyPipeline";

namespace btspeaker {

    SpotifyPipeline::SpotifyPipeline()
            : isPlaying(false),
              isInitialized(false),
              isRunning(false),
              volume(100),
              isAuthenticated(false),
              httpServer(nullptr) {
        ESP_LOGI(TAG, "SpotifyPipeline created");
    }

    SpotifyPipeline::~SpotifyPipeline() {
        stop();
        ESP_LOGI(TAG, "SpotifyPipeline destroyed");
    }

    esp_err_t SpotifyPipeline::initialize() {
        if (isInitialized) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Initializing Spotify pipeline with persistent authentication");

        // Try to load existing authentication
        esp_err_t auth_ret = loadStoredAuth();
        if (auth_ret == ESP_OK) {
            ESP_LOGI(TAG, "Loaded stored authentication for user: %s", storedUsername.c_str());
            isAuthenticated = true;
        } else {
            ESP_LOGI(TAG, "No stored authentication found, will need to authenticate");
        }

        // Initialize audio components
        esp_err_t ret = initializeAudioComponents();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize audio components: %s", esp_err_to_name(ret));
            return ret;
        }

        isInitialized = true;
        ESP_LOGI(TAG, "Spotify pipeline initialized successfully");
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::loadStoredAuth() {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "No stored authentication namespace found");
            return ESP_FAIL;
        }

        // Get size of stored auth blob
        size_t required_size = 0;
        err = nvs_get_blob(nvs_handle, NVS_AUTH_KEY, NULL, &required_size);
        if (err != ESP_OK || required_size == 0) {
            ESP_LOGD(TAG, "No stored authentication blob found");
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }

        // Read the stored JSON auth blob
        std::vector<char> auth_json(required_size + 1);
        err = nvs_get_blob(nvs_handle, NVS_AUTH_KEY, auth_json.data(), &required_size);
        nvs_close(nvs_handle);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read stored authentication: %s", esp_err_to_name(err));
            return ESP_FAIL;
        }

        auth_json[required_size] = '\0'; // Null terminate

        try {
            // Create login blob and load from JSON
            loginBlob = std::make_shared<cspot::LoginBlob>(SPOTIFY_DEVICE_NAME);
            loginBlob->loadJson(std::string(auth_json.data()));
            storedUsername = loginBlob->getUserName();

            ESP_LOGI(TAG, "Successfully loaded stored authentication for: %s", storedUsername.c_str());
            return ESP_OK;

        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Failed to parse stored authentication: %s", e.what());
            return ESP_FAIL;
        }
    }

    esp_err_t SpotifyPipeline::saveAuthBlob() {
        if (!loginBlob || loginBlob->authData.empty()) {
            ESP_LOGE(TAG, "No authentication data to save");
            return ESP_FAIL;
        }

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
            return err;
        }

        try {
            std::string auth_json = loginBlob->toJson();
            err = nvs_set_blob(nvs_handle, NVS_AUTH_KEY, auth_json.c_str(), auth_json.length());

            if (err == ESP_OK) {
                err = nvs_commit(nvs_handle);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Successfully saved authentication for user: %s", loginBlob->getUserName().c_str());
                    storedUsername = loginBlob->getUserName();
                }
            }

        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Failed to serialize authentication: %s", e.what());
            err = ESP_FAIL;
        }

        nvs_close(nvs_handle);
        return err;
    }

    void SpotifyPipeline::clearStoredAuth() {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_erase_key(nvs_handle, NVS_AUTH_KEY);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Cleared stored Spotify authentication");
        }

        isAuthenticated = false;
        storedUsername = "";
        loginBlob.reset();
    }

    esp_err_t SpotifyPipeline::startZeroconfDiscovery() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting Spotify Connect Zeroconf discovery...");
        ESP_LOGI(TAG, "Open Spotify app and look \"for Rupert's Revenge\" in Connect devices");

        // Create fresh login blob for pairing
        loginBlob = std::make_shared<cspot::LoginBlob>(SPOTIFY_DEVICE_NAME);

        // Start HTTP server first
        esp_err_t ret = startHttpServer();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            clearStoredAuth();
            return ret;
        }

        // Set up mDNS service for Spotify discovery
        ret = mdns_service_add(NULL, SPOTIFY_SERVICE_TYPE, SPOTIFY_PROTOCOL, SPOTIFY_CONNECT_PORT, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add mDNS service: %s", esp_err_to_name(ret));
            stopHttpServer();
            return ret;
        }

        // Add required TXT records for Spotify Connect
        mdns_txt_item_t spotify_txt[] = {
                {"VERSION", "1.0"},
                {"CPath",   "/spotify_info"},  // This is crucial - points to our HTTP endpoint
                {"Stack",   "SP"},
        };
        ret = mdns_service_txt_set(SPOTIFY_SERVICE_TYPE, SPOTIFY_PROTOCOL, spotify_txt, 3);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set mDNS TXT records: %s", esp_err_to_name(ret));
            stopHttpServer();
            return ret;
        }

        ESP_LOGI(TAG, "Spotify Zeroconf discovery started on port %d", SPOTIFY_CONNECT_PORT);
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::startHttpServer() {
        if (httpServer != nullptr) {
            ESP_LOGW(TAG, "HTTP server already running");
            return ESP_OK;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = SPOTIFY_CONNECT_PORT;
        config.ctrl_port = SPOTIFY_CONNECT_PORT + 1;
        config.max_uri_handlers = 8;
        config.max_resp_headers = 8;
        config.stack_size = 8192;

        ESP_LOGI(TAG, "Starting HTTP server on port %d", SPOTIFY_CONNECT_PORT);

        esp_err_t ret = httpd_start(&httpServer, &config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            clearStoredAuth();
            return ret;
        }

        // Register GET /spotify_info handler
        httpd_uri_t spotify_info_get = {
                .uri = "/spotify_info",
                .method = HTTP_GET,
                .handler = spotifyInfoGetHandler,
                .user_ctx = this
        };

        httpd_uri_t spotify_info_post = {
                .uri = "/spotify_info",
                .method = HTTP_POST,
                .handler = spotifyInfoPostHandler,
                .user_ctx = this
        };

        httpd_uri_t close_uri = {
                .uri = "/close",
                .method = HTTP_GET,
                .handler = spotifyInfoGetHandler, // Reuse the get handler for close
                .user_ctx = this
        };

        // Register handlers
        httpd_register_uri_handler(httpServer, &spotify_info_get);
        httpd_register_uri_handler(httpServer, &spotify_info_post);
        httpd_register_uri_handler(httpServer, &close_uri);

        ESP_LOGI(TAG, "HTTP server started on port %d", SPOTIFY_CONNECT_PORT);
        return ESP_OK;
    }

    void SpotifyPipeline::stopHttpServer() {
        if (httpServer) {
            ESP_LOGI(TAG, "Stopping HTTP server");
            httpd_stop(httpServer);
            httpServer = nullptr;
        }
    }

// Static HTTP handlers
    esp_err_t SpotifyPipeline::spotifyInfoGetHandler(httpd_req_t *req) {
        SpotifyPipeline *pipeline = getInstanceFromRequest(req);
        if (!pipeline || !pipeline->loginBlob) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "GET /spotify_info request received");

        try {
            // Build Zeroconf info JSON using cspot's method
            std::string info_json = pipeline->loginBlob->buildZeroconfInfo();

            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, info_json.c_str(), info_json.length());

            ESP_LOGD(TAG, "Sent device info: %s", info_json.c_str());
            return ESP_OK;

        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Failed to build Zeroconf info: %s", e.what());
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    esp_err_t SpotifyPipeline::spotifyInfoPostHandler(httpd_req_t *req) {
        SpotifyPipeline *pipeline = getInstanceFromRequest(req);
        if (!pipeline || !pipeline->loginBlob) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "POST /spotify_info request received - Spotify app connecting!");

        // Read the request body
        char *content = nullptr;
        size_t content_len = req->content_len;

        if (content_len > 0) {
            content = (char *) malloc(content_len + 1);
            if (!content) {
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }

            int ret = httpd_req_recv(req, content, content_len);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    httpd_resp_send_408(req);
                } else {
                    httpd_resp_send_500(req);
                }
                free(content);
                return ESP_FAIL;
            }
            content[content_len] = '\0';

            ESP_LOGI(TAG, "Received auth data: %s", content);

            try {
                // Parse the form data and load into login blob
                std::map<std::string, std::string> queryMap;

                // Simple form parsing (for production, use a proper parser)
                char *token = strtok(content, "&");
                while (token != nullptr) {
                    char *eq = strchr(token, '=');
                    if (eq != nullptr) {
                        *eq = '\0';
                        std::string key = token;
                        std::string value = eq + 1;
                        queryMap[key] = value;
                    }
                    token = strtok(nullptr, "&");
                }

                // Load the Zeroconf query into the login blob
                pipeline->loginBlob->loadZeroconfQuery(queryMap);

                // Send success response to Spotify app
                cJSON *response = cJSON_CreateObject();
                cJSON_AddNumberToObject(response, "status", 101);
                cJSON_AddNumberToObject(response, "spotifyError", 0);
                cJSON_AddStringToObject(response, "statusString", "ERROR-OK");

                char *response_str = cJSON_Print(response);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, response_str, strlen(response_str));

                free(response_str);
                cJSON_Delete(response);
                free(content);

                // Notify the pipeline that authentication is complete
                ESP_LOGI(TAG, "Authentication data received, processing...");
                pipeline->onSpotifyConnected(pipeline->loginBlob);

                return ESP_OK;

            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "Failed to process authentication: %s", e.what());
                free(content);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }

        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }

    SpotifyPipeline *SpotifyPipeline::getInstanceFromRequest(httpd_req_t *req) {
        return static_cast<SpotifyPipeline *>(req->user_ctx);
    }

    esp_err_t SpotifyPipeline::start() {
        if (!isInitialized) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        if (isRunning) {
            ESP_LOGW(TAG, "Already running");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Starting Spotify pipeline");

        // If we have stored authentication, try to use it
        if (isAuthenticated && loginBlob) {
            ESP_LOGI(TAG, "Using stored authentication for user: %s", storedUsername.c_str());
            esp_err_t ret = startAuthentication();
            if (ret == ESP_OK) {
                isRunning = true;
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Stored authentication failed, clearing and starting discovery");
                clearStoredAuth();
            }
        }

        // No authentication or authentication failed - start Zeroconf discovery
        if (!isAuthenticated || !loginBlob) {
            ESP_LOGI(TAG, "Starting Zeroconf discovery for initial pairing");
            esp_err_t ret = startZeroconfDiscovery();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start Zeroconf discovery: %s", esp_err_to_name(ret));
                return ret;
            }

            // Wait for authentication via Spotify app
            ESP_LOGI(TAG, "Waiting for Spotify app pairing...");
            ESP_LOGI(TAG, "Instructions:");
            ESP_LOGI(TAG, "1. Open Spotify app on your phone");
            ESP_LOGI(TAG, "2. Start playing music");
            ESP_LOGI(TAG, "3. Tap the Connect icon");
            ESP_LOGI(TAG, "4. Select 'ESP32_SPEAKER' from the list");

            // Set up a task to handle the authentication when it comes
            isRunning = true;
            return ESP_OK;
        }

        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t SpotifyPipeline::startAuthentication() {
        if (!loginBlob) {
            ESP_LOGE(TAG, "No login blob available");
            return ESP_ERR_INVALID_STATE;
        }

        try {
            // Create Spotify context with our stored credentials
            context = cspot::Context::createFromBlob(loginBlob);

            if (!context) {
                ESP_LOGE(TAG, "Failed to create Spotify context from blob");
                return ESP_FAIL;
            }

            // Set up device state handler with disconnect callback
            deviceStateHandler = std::make_shared<cspot::DeviceStateHandler>(context, [this]() {
                ESP_LOGI(TAG, "Spotify session disconnected");
                isRunning = false;
            });

            // Set up command callback
            deviceStateHandler->stateCallback = [this](const cspot::DeviceStateHandler::Command &command) {
                handleSpotifyCommand(command);
            };

            // Start audio streaming now that we have a device handler
            esp_err_t audio_ret = startAudioStreaming();
            if (audio_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start audio streaming: %s", esp_err_to_name(audio_ret));
                return audio_ret;
            }

            ESP_LOGI(TAG, "Successfully authenticated with Spotify and started audio streaming");
            isAuthenticated = true;
            return ESP_OK;

        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Authentication failed: %s", e.what());
            return ESP_FAIL;
        }
    }

    void SpotifyPipeline::handleSpotifyCommand(const cspot::DeviceStateHandler::Command &command) {
        ESP_LOGI(TAG, "Received Spotify command: %d", static_cast<int>(command.commandType));

        switch (command.commandType) {
            case cspot::DeviceStateHandler::CommandType::PLAY:
                ESP_LOGI(TAG, "Play command received");
                isPlaying = true;
                break;

            case cspot::DeviceStateHandler::CommandType::PAUSE:
                ESP_LOGI(TAG, "Pause command received");
                isPlaying = false;
                break;

            case cspot::DeviceStateHandler::CommandType::STOP:
                ESP_LOGI(TAG, "Stop command received");
                isPlaying = false;
                break;

            case cspot::DeviceStateHandler::CommandType::SKIP_NEXT:
                ESP_LOGI(TAG, "Skip next command received");
                break;

            case cspot::DeviceStateHandler::CommandType::SKIP_PREV:
                ESP_LOGI(TAG, "Skip previous command received");
                break;

            case cspot::DeviceStateHandler::CommandType::VOLUME:
                ESP_LOGI(TAG, "Volume command received");
                if (std::holds_alternative<int32_t>(command.data)) {
                    volume = static_cast<uint8_t>(std::get<int32_t>(command.data));
                    ESP_LOGI(TAG, "Volume set to: %d", volume.load());
                }
                break;

            case cspot::DeviceStateHandler::CommandType::SEEK:
                ESP_LOGI(TAG, "Seek command received");
                break;

            case cspot::DeviceStateHandler::CommandType::TRACK_INFO:
                ESP_LOGI(TAG, "Track info command received");
                break;

            case cspot::DeviceStateHandler::CommandType::PLAYBACK_START:
                ESP_LOGI(TAG, "Playback start command received");
                isPlaying = true;
                break;

            case cspot::DeviceStateHandler::CommandType::PLAYBACK:
                ESP_LOGI(TAG, "Playback command received");
                break;

            case cspot::DeviceStateHandler::CommandType::DEPLETED:
                ESP_LOGI(TAG, "Track depleted command received");
                break;

            case cspot::DeviceStateHandler::CommandType::FLUSH:
                ESP_LOGI(TAG, "Flush command received");
                break;

            case cspot::DeviceStateHandler::CommandType::DISC:
                ESP_LOGI(TAG, "Disconnect command received");
                isRunning = false;
                break;

            case cspot::DeviceStateHandler::CommandType::SET_SHUFFLE:
                ESP_LOGI(TAG, "Set shuffle command received");
                break;

            case cspot::DeviceStateHandler::CommandType::SET_REPEAT:
                ESP_LOGI(TAG, "Set repeat command received");
                break;

            default:
                ESP_LOGW(TAG, "Unknown command type: %d", static_cast<int>(command.commandType));
                break;
        }
    }

    esp_err_t SpotifyPipeline::stop() {
        if (!isInitialized) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Stopping Spotify pipeline");
        isRunning = false;
        isPlaying = false;

        // Stop audio streaming first
        stopAudioStreaming();

        // Stop HTTP server and mDNS service
        stopHttpServer();

        if (deviceStateHandler) {
            deviceStateHandler->disconnect();
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::pause() {
        if (!isInitialized || !isPlaying) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Pausing Spotify playback");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PAUSE);
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::resume() {
        if (!isInitialized || isPlaying) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Resuming Spotify playback");
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
        }
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::playURI(const std::string &uri) {
        if (!deviceStateHandler || !context) {
            ESP_LOGE(TAG, "Device handler or context not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "[play] Playing Spotify URI: %s", uri.c_str());

        // Clear current queue
        deviceStateHandler->currentTracks.clear();

        // Create ProvidedTrack for the URI
        ProvidedTrack track = ProvidedTrack_init_zero;
        track.uri = strdup(uri.c_str());
        track.provider = strdup("queue");
        deviceStateHandler->currentTracks.push_back(track);
        deviceStateHandler->offset = 0;

        // Reset preloaded tracks
        if (deviceStateHandler->trackQueue) {
            deviceStateHandler->trackQueue->preloadedTracks.clear();
        }

        // Trigger playback
        deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
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

        // Apply volume to audio sink if available
        if (audioSink) {
            // PCM5102AudioSink doesn't have volume control, so we just store it
            // Volume control would be handled by the amplifier or digitally
        }
    }

// This is called when the Spotify app connects to our device via Zeroconf
    esp_err_t SpotifyPipeline::onSpotifyConnected(const std::shared_ptr<cspot::LoginBlob> &blob) {
        if (!blob) {
            ESP_LOGE(TAG, "Invalid login blob received");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Spotify app connected! Saving authentication...");

        loginBlob = blob;
        storedUsername = blob->getUserName();

        // Save authentication for future use
        esp_err_t ret = saveAuthBlob();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save authentication: %s", esp_err_to_name(ret));
        }

        ESP_LOGI(TAG, "Spotify authentication successful for user: %s", storedUsername.c_str());
        isAuthenticated = true;

        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::initializeAudioComponents() {
        ESP_LOGI(TAG, "Initializing audio streaming components");

        // Create PCM5102A audio sink
        audioSink = std::make_shared<PCM5102AudioSink>();
        if (!audioSink) {
            ESP_LOGE(TAG, "Failed to create PCM5102AudioSink");
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "PCM5102AudioSink created successfully");
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::startAudioStreaming() {
        if (!audioSink || !deviceStateHandler) {
            ESP_LOGE(TAG, "Audio components not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting audio streaming with EspPlayer");

        // Create EspPlayer to handle audio streaming from cspot to PCM5102A
        espPlayer = std::make_unique<EspPlayer>(audioSink, deviceStateHandler);
        if (!espPlayer) {
            ESP_LOGE(TAG, "Failed to create EspPlayer");
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "Audio streaming started successfully");
        return ESP_OK;
    }

    esp_err_t SpotifyPipeline::stopAudioStreaming() {
        if (espPlayer) {
            ESP_LOGI(TAG, "Stopping audio streaming");
            espPlayer.reset();
        }
        return ESP_OK;
    }

} // namespace btspeaker 