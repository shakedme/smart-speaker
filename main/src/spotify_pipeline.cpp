#include "../include/spotify_pipeline.hpp"
#include "../include/spotify_config.hpp"
#include "../include/adf_audio_sink.hpp"

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

    SpotifyPipeline::SpotifyPipeline()
            : isPlaying(false),
              isInitialized(false),
              isRunning_(false),
              volume(100),
              isAuthenticated(false),
              httpServer(nullptr) {
        ESP_LOGI(TAG, "Initializing Spotify pipeline");

        audioSink = std::make_shared<ADFAudioSink>();

        clearStoredAuth();

        // No authentication or authentication failed - start Zeroconf discovery
        if (!isAuthenticated) {
            ESP_LOGI(TAG, "Starting Zeroconf discovery for initial pairing");
            esp_err_t ret = startZeroconfDiscovery();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start Zeroconf discovery: %s", esp_err_to_name(ret));
                throw std::runtime_error("Failed to start Zeroconf discovery");
            }
        }

        isInitialized = true;
        ESP_LOGI(TAG, "Spotify pipeline initialized successfully");
    }

    SpotifyPipeline::~SpotifyPipeline() {
        stop();

        // Ensure packet handler task is fully cleaned up
        if (packetHandlerTaskHandle != NULL) {
            ESP_LOGW(TAG, "Force cleaning up packet handler task in destructor");
            vTaskDelete(packetHandlerTaskHandle);
            packetHandlerTaskHandle = NULL;
        }

        // Clean up any allocated memory
        if (taskStack) {
            heap_caps_free(taskStack);
            taskStack = nullptr;
        }
        if (taskBuffer) {
            heap_caps_free(taskBuffer);
            taskBuffer = nullptr;
        }

        ESP_LOGI(TAG, "SpotifyPipeline destroyed");
    }

        esp_err_t SpotifyPipeline::start() {
        if (!isInitialized) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        if (isRunning_) {
            ESP_LOGW(TAG, "Already running");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Starting Spotify pipeline in dedicated task (like official example)");
        
        const size_t stackSizeWords = 8192 * 4;  // 32KB stack
        taskStack = (StackType_t *) heap_caps_malloc(
            stackSizeWords * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        
        taskBuffer = (StaticTask_t *) heap_caps_malloc(
            sizeof(StaticTask_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
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
        
        ESP_LOGI(TAG, "Allocated 2MB Spotify task stack in PSRAM: %p, buffer in IRAM: %p", taskStack, taskBuffer);
        
        packetHandlerTaskHandle = xTaskCreateStaticPinnedToCore(
            spotifyMainTask,  // Main Spotify task (like CSpotTask::runTask)
            "spotify_main",
            stackSizeWords,   // 2MB stack for entire workflow
            this,
            3,                // Priority
            taskStack,
            taskBuffer,
            0                 // Core 0
        );
        
        if (packetHandlerTaskHandle == NULL) {
            ESP_LOGE(TAG, "Failed to create Spotify main task");
            heap_caps_free(taskStack);
            heap_caps_free(taskBuffer);
            taskStack = nullptr;
            taskBuffer = nullptr;
            return ESP_FAIL;
        }

        isRunning_ = true;
        ESP_LOGI(TAG, "Spotify main task started");
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
            loginBlob = std::make_shared<cspot::LoginBlob>(std::string(btspeaker::SPOTIFY_DEVICE_NAME));
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
        ESP_LOGI(TAG, "Starting Spotify Connect Zeroconf discovery...");
        ESP_LOGI(TAG, "Open Spotify app and look ESP32_SPEAKER in Connect devices");

        // Create fresh login blob for pairing
        loginBlob = std::make_shared<cspot::LoginBlob>(std::string(btspeaker::SPOTIFY_DEVICE_NAME));

        // Start HTTP server first
        esp_err_t ret = startHttpServer();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            clearStoredAuth();
            return ret;
        }

        // Set up mDNS service for Spotify discovery
        mdns_init();
        mdns_hostname_set("cspot");
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
        config.stack_size = 16384;  // Increased for TLS operations

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
                .handler = closeHandler,
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

                // Use std::string for safer parsing without modifying original buffer
                std::string content_str(content);
                size_t start = 0;

                while (start < content_str.length()) {
                    size_t amp_pos = content_str.find('&', start);
                    if (amp_pos == std::string::npos) {
                        amp_pos = content_str.length();
                    }

                    std::string param = content_str.substr(start, amp_pos - start);
                    size_t eq_pos = param.find('=');

                    if (eq_pos != std::string::npos) {
                        std::string key = param.substr(0, eq_pos);
                        std::string value = urlDecode(param.substr(eq_pos + 1));
                        queryMap[key] = value;
                        ESP_LOGD(TAG, "Parsed param: %s = %s", key.c_str(), value.c_str());
                    }

                    start = amp_pos + 1;
                }

                // Validate critical parameters before processing
                if (queryMap.find("blob") == queryMap.end() || queryMap.find("clientKey") == queryMap.end()) {
                    ESP_LOGE(TAG, "Missing critical authentication parameters");
                    free(content);
                    httpd_resp_send_500(req);
                    return ESP_FAIL;
                }

                ESP_LOGI(TAG, "Authentication parameters validated, loading into LoginBlob...");

                // Load the Zeroconf query into the login blob
                pipeline->loginBlob->loadZeroconfQuery(queryMap);

                ESP_LOGI(TAG, "LoginBlob loaded successfully");

                // Send success response to Spotify app IMMEDIATELY
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

                ESP_LOGI(TAG, "Authentication data received, deferring processing to separate task...");

                // Validate that the LoginBlob is still valid
                if (!pipeline->loginBlob || pipeline->loginBlob->authData.empty()) {
                    ESP_LOGE(TAG, "LoginBlob became invalid during processing");
                    return ESP_OK; // Response already sent
                }

                // Defer authentication to a separate task to avoid TLS operations in HTTP server context
                xTaskCreate([](void *param) {
                    SpotifyPipeline *pipeline = static_cast<SpotifyPipeline *>(param);
                    esp_err_t auth_result = pipeline->onSpotifyConnected(pipeline->loginBlob);
                    if (auth_result != ESP_OK) {
                        ESP_LOGW(TAG, "Authentication processing failed");
                    } else {
                        pipeline->authenticatedCallback();
                    }
                    vTaskDelete(NULL);
                }, "spotify_auth", 16384, pipeline, 5, NULL);

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

    esp_err_t SpotifyPipeline::closeHandler(httpd_req_t *req) {
        ESP_LOGI(TAG, "GET /close request received");

        // Send empty response as per Spotify Connect protocol
        httpd_resp_send(req, "", 0);

        return ESP_OK;
    }

    SpotifyPipeline *SpotifyPipeline::getInstanceFromRequest(httpd_req_t *req) {
        return static_cast<SpotifyPipeline *>(req->user_ctx);
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

            ESP_LOGI(TAG, "Connecting to Spotify servers...");

            vTaskDelay(pdMS_TO_TICKS(1000));

            context->session->connectWithRandomAp();
            context->config.authData = context->session->authenticate(loginBlob);

            if (context->config.authData.size() == 0) {
                ESP_LOGE(TAG, "Authentication failed - no auth data received");
                return ESP_FAIL;
            }

            // Create DeviceStateHandler AFTER authentication completes (like official example)
            deviceStateHandler = std::make_shared<cspot::DeviceStateHandler>(context, [this]() {
                ESP_LOGI(TAG, "Spotify session disconnected");
                isRunning_ = false;
            });

            isAuthenticated = true;
            ESP_LOGI(TAG, "Successfully authenticated with Spotify servers");

            return ESP_OK;
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Authentication failed: %s", e.what());
            return ESP_FAIL;
        }
    }

    esp_err_t SpotifyPipeline::stop() {
        if (!isInitialized) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Stopping Spotify pipeline");
        isRunning_ = false;
        isPlaying = false;

        // Wait for packet handler task to finish (if we created one)
        if (packetHandlerTaskHandle != NULL) {
            ESP_LOGI(TAG, "Waiting for packet handler task to finish...");

            // Give the task some time to exit gracefully
            vTaskDelay(pdMS_TO_TICKS(500));

            // Check if task is still running and force delete if necessary
            if (eTaskGetState(packetHandlerTaskHandle) != eDeleted) {
                ESP_LOGW(TAG, "Force deleting packet handler task");
                vTaskDelete(packetHandlerTaskHandle);
            }

            packetHandlerTaskHandle = NULL;
        } else {
            ESP_LOGI(TAG, "No separate packet handler task to clean up");
        }

        // Clean up allocated memory
        if (taskStack) {
            heap_caps_free(taskStack);
            taskStack = nullptr;
        }
        if (taskBuffer) {
            heap_caps_free(taskBuffer);
            taskBuffer = nullptr;
        }

        if (deviceStateHandler) {
            deviceStateHandler->disconnect();
        }
        audioSink->stop();

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

        audioSink->resume();
        if (deviceStateHandler) {
            deviceStateHandler->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
            isPlaying = true;
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

        ESP_LOGI(TAG, "Spotify app connected! Processing authentication...");

        loginBlob = blob;
        storedUsername = blob->getUserName();

//        // Save authentication for future use
//        esp_err_t ret = saveAuthBlob();
//        if (ret != ESP_OK) {
//            ESP_LOGW(TAG, "Failed to save authentication: %s", esp_err_to_name(ret));
//        }

        // Now actually connect to Spotify servers and authenticate
        esp_err_t ret = startAuthentication();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Spotify authentication: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Spotify authentication complete for user: %s", storedUsername.c_str());

        // Clean up HTTP server and mDNS after successful authentication
        stopHttpServer();
        mdns_free();
        ESP_LOGI(TAG, "Cleaned up HTTP server and mDNS after authentication");

        return ESP_OK;
    }


    std::string SpotifyPipeline::urlDecode(const std::string &encoded) {
        std::string decoded;
        decoded.reserve(encoded.length());

        for (size_t i = 0; i < encoded.length(); ++i) {
            if (encoded[i] == '%') {
                // Handle percent-encoded characters
                if (i + 2 < encoded.length()) {
                    std::string hex = encoded.substr(i + 1, 2);
                    try {
                        int charValue = std::stoi(hex, nullptr, 16);
                        decoded += static_cast<char>(charValue);
                        i += 2; // Skip the two hex digits
                    } catch (const std::invalid_argument &e) {
                        // Handle invalid hex sequence, keep the '%' as-is
                        decoded += '%';
                        ESP_LOGW(TAG, "Invalid hex sequence in URL decode: %s", hex.c_str());
                    } catch (const std::out_of_range &e) {
                        // Handle out of range values, keep the '%' as-is
                        decoded += '%';
                        ESP_LOGW(TAG, "Out of range hex value in URL decode: %s", hex.c_str());
                    }
                } else {
                    // Incomplete percent-encoding, keep the '%' as-is
                    decoded += '%';
                }
            } else if (encoded[i] == '+') {
                // Convert '+' to space
                decoded += ' ';
            } else {
                // Keep other characters as they are
                decoded += encoded[i];
            }
        }
        return decoded;
    }

    void SpotifyPipeline::spotifyMainTask(void *parameter) {
        SpotifyPipeline *pipeline = static_cast<SpotifyPipeline *>(parameter);

        ESP_LOGI(TAG, "Spotify main task started (like official CSpotTask)");

        if (!pipeline) {
            ESP_LOGE(TAG, "Invalid pipeline parameter in task");
            vTaskDelete(NULL);
            return;
        }

        try {
            // Wait for authentication to complete
            while (!pipeline->isAuthenticated && pipeline->isRunning()) {
                ESP_LOGD(TAG, "Waiting for authentication to complete...");
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            if (!pipeline->isAuthenticated || !pipeline->isRunning()) {
                ESP_LOGW(TAG, "Authentication not completed or pipeline stopped");
                vTaskDelete(NULL);
                return;
            }

            // Now do the authenticated workflow (like official example)
            ESP_LOGI(TAG, "Authentication complete, starting Spotify session");

            // Start session task AFTER authentication (like official example)
            pipeline->context->session->startTask();

            // Start audio sink
            ESP_LOGI(TAG, "Starting audio sink...");
            pipeline->audioSink->start();
            ESP_LOGI(TAG, "Audio sink started successfully");
            
            // Create EspPlayer AFTER session task starts (like official example)
            ESP_LOGI(TAG, "Creating EspPlayer...");
            pipeline->espPlayer = std::make_unique<EspPlayer>(pipeline->audioSink, pipeline->deviceStateHandler);
            ESP_LOGI(TAG, "EspPlayer created successfully");

            MemoryUtils::logMemoryStats("SpotifyPipeline::mainTask");
            
            ESP_LOGI(TAG, "Starting packet handling loop");

            // Main packet handling loop (like official example)
            while (pipeline->isRunning()) {
                if (pipeline->context && pipeline->context->session) {
                    try {
                        pipeline->context->session->handlePacket();
                    } catch (const std::exception &e) {
                        ESP_LOGE(TAG, "Exception processing packet: %s", e.what());
                        // Check if this is a critical error that should stop the task
                        std::string error_msg = e.what();
                        if (error_msg.find("Connection failed") != std::string::npos ||
                            error_msg.find("SSL handshake failed") != std::string::npos ||
                            error_msg.find("network") != std::string::npos) {
                            ESP_LOGE(TAG, "Critical network error detected, restarting session...");
                            vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds before retry
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

        ESP_LOGI(TAG, "Spotify main task exiting");
        pipeline->isRunning_ = false;
        vTaskDelete(NULL);
    }


} // namespace btspeaker