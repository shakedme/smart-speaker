#include "../include/spotify_auth_manager.hpp"
#include "../include/spotify_config.hpp"

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <cstring>

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "nvs.h"
#include "mdns.h"
}

namespace btspeaker {

    static const char *TAG = "SpotifyAuthManager";

    SpotifyAuthManager::SpotifyAuthManager()
            : isAuthenticated_(false) {}

    SpotifyAuthManager::~SpotifyAuthManager() {
        stopHttpServer();
        stopZeroconfDiscovery();
    }

    esp_err_t SpotifyAuthManager::loadStoredAuth() {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "No stored authentication namespace found");
            return ESP_FAIL;
        }
        size_t required_size = 0;
        err = nvs_get_blob(nvs_handle, NVS_AUTH_KEY, NULL, &required_size);
        if (err != ESP_OK || required_size == 0) {
            ESP_LOGD(TAG, "No stored authentication blob found");
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }
        std::vector<char> auth_json(required_size + 1);
        err = nvs_get_blob(nvs_handle, NVS_AUTH_KEY, auth_json.data(), &required_size);
        nvs_close(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read stored authentication: %s", esp_err_to_name(err));
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Loaded stored authentication: %s", auth_json.data());
        auth_json[required_size] = '\0';
        
        // Debug: Check the raw JSON string before parsing
        std::string json_str(auth_json.data());
        ESP_LOGI(TAG, "🔧 DEBUG: Raw JSON string length: %d", json_str.length());
        ESP_LOGI(TAG, "🔧 DEBUG: Raw JSON content: '%s'", json_str.c_str());
        
        try {
            loginBlob_ = std::make_shared<cspot::LoginBlob>(std::string(btspeaker::SPOTIFY_DEVICE_NAME));
            ESP_LOGI(TAG, "🔧 DEBUG: Created new LoginBlob, loading JSON...");
            
            // Debug: Check blob state right after creation but before JSON load
            ESP_LOGI(TAG, "🔧 DEBUG: Blob before JSON load - Username: '%s', AuthType: %d, AuthData size: %d", 
                     loginBlob_->getUserName().c_str(), loginBlob_->authType, loginBlob_->authData.size());
            
            loginBlob_->loadJson(json_str);
            ESP_LOGI(TAG, "🔧 DEBUG: JSON loaded successfully");
            
            // Debug: Check blob state immediately after JSON load
            ESP_LOGI(TAG, "🔧 DEBUG: Blob after JSON load - Username: '%s', AuthType: %d, AuthData size: %d", 
                     loginBlob_->getUserName().c_str(), loginBlob_->authType, loginBlob_->authData.size());
            
            storedUsername_ = loginBlob_->getUserName();
            isAuthenticated_ = true;
            ESP_LOGI(TAG, "Successfully loaded stored authentication for: %s", storedUsername_.c_str());
            
            // Add detailed debugging of loaded blob
            ESP_LOGI(TAG, "🔧 DEBUG: Loaded blob details:");
            ESP_LOGI(TAG, "  - Username: %s", loginBlob_->getUserName().c_str());
            ESP_LOGI(TAG, "  - Device ID: %s", loginBlob_->getDeviceId().c_str());
            ESP_LOGI(TAG, "  - Device Name: %s", loginBlob_->getDeviceName().c_str());
            ESP_LOGI(TAG, "  - Auth Type: %d", loginBlob_->authType);
            ESP_LOGI(TAG, "  - Auth Data Size: %d bytes", loginBlob_->authData.size());
            
            // Validate the loaded auth
            if (!isStoredAuthValid()) {
                ESP_LOGW(TAG, "Loaded auth blob failed validation, clearing...");
                clearStoredAuth();
                return ESP_FAIL;
            }
            
            return ESP_OK;
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Failed to parse stored authentication: %s", e.what());
            ESP_LOGI(TAG, "Clearing corrupted stored auth...");
            clearStoredAuth();
            return ESP_FAIL;
        }
    }

    esp_err_t SpotifyAuthManager::saveAuthBlob() {
        if (!loginBlob_ || loginBlob_->authData.empty()) {
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
            std::string auth_json = loginBlob_->toJson();
            err = nvs_set_blob(nvs_handle, NVS_AUTH_KEY, auth_json.c_str(), auth_json.length());
            if (err == ESP_OK) {
                err = nvs_commit(nvs_handle);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Successfully saved authentication for user: %s", loginBlob_->getUserName().c_str());
                    storedUsername_ = loginBlob_->getUserName();
                }
            }
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Failed to serialize authentication: %s", e.what());
            err = ESP_FAIL;
        }
        nvs_close(nvs_handle);
        return err;
    }

    void SpotifyAuthManager::clearStoredAuth() {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_erase_key(nvs_handle, NVS_AUTH_KEY);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Cleared stored Spotify authentication");
        }
        isAuthenticated_ = false;
        storedUsername_.clear();
        loginBlob_.reset();
    }

    esp_err_t SpotifyAuthManager::startZeroconfDiscovery() {
        ESP_LOGI(TAG, "Starting Spotify Connect Zeroconf discovery...");
        ESP_LOGI(TAG, "Open Spotify app and look Rufus in Connect devices");

        esp_err_t ret = startHttpServer();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            clearStoredAuth();
            return ret;
        }
        mdns_init();
        mdns_hostname_set("cspot");
        ret = mdns_service_add(NULL, SPOTIFY_SERVICE_TYPE, SPOTIFY_PROTOCOL, SPOTIFY_CONNECT_PORT, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add mDNS service: %s", esp_err_to_name(ret));
            stopHttpServer();
            return ret;
        }
        mdns_txt_item_t spotify_txt[] = {
                {"VERSION", "1.0"},
                {"CPath",   "/spotify_info"},
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

    void SpotifyAuthManager::stopZeroconfDiscovery() {
        mdns_free();
    }

    esp_err_t SpotifyAuthManager::startHttpServer() {
        if (httpServer_ != nullptr) {
            ESP_LOGW(TAG, "HTTP server already running");
            return ESP_OK;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = SPOTIFY_CONNECT_PORT;
        config.ctrl_port = SPOTIFY_CONNECT_PORT + 1;
        config.max_uri_handlers = 8;
        config.max_resp_headers = 8;
        config.stack_size = 16384;
        config.task_caps = MALLOC_CAP_SPIRAM;

        ESP_LOGI(TAG, "Starting HTTP server on port %d with stack size %d (PSRAM)", SPOTIFY_CONNECT_PORT,
                 config.stack_size);
        esp_err_t ret = httpd_start(&httpServer_, &config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            clearStoredAuth();
            return ret;
        }
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
        httpd_register_uri_handler(httpServer_, &spotify_info_get);
        httpd_register_uri_handler(httpServer_, &spotify_info_post);
        httpd_register_uri_handler(httpServer_, &close_uri);
        ESP_LOGI(TAG, "HTTP server started on port %d", SPOTIFY_CONNECT_PORT);
        return ESP_OK;
    }

    void SpotifyAuthManager::stopHttpServer() {
        if (httpServer_) {
            ESP_LOGI(TAG, "Stopping HTTP server");
            httpd_stop(httpServer_);
            httpServer_ = nullptr;
        }
    }

    esp_err_t SpotifyAuthManager::spotifyInfoGetHandler(httpd_req_t *req) {
        SpotifyAuthManager *mgr = getInstanceFromRequest(req);
        if (!mgr || !mgr->loginBlob_) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "GET /spotify_info request received");
        try {
            std::string info_json = mgr->loginBlob_->buildZeroconfInfo();
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

    esp_err_t SpotifyAuthManager::spotifyInfoPostHandler(httpd_req_t *req) {
        SpotifyAuthManager *mgr = getInstanceFromRequest(req);
        if (!mgr || !mgr->loginBlob_) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "POST /spotify_info request received - Spotify app connecting!");
        if (mgr->isAuthenticated()) {

        }
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

            try {
                std::string content_str(content);
                auto queryMap = parseFormData(content_str);
                if (!validateAuthParams(queryMap)) {
                    ESP_LOGE(TAG, "Missing critical authentication parameters");
                    free(content);
                    httpd_resp_send_500(req);
                    return ESP_FAIL;
                }
                mgr->loginBlob_->loadZeroconfQuery(queryMap);
                ESP_LOGI(TAG, "LoginBlob loaded successfully");

                sendAuthSuccessResponse(req);
                free(content);
                if (!mgr->loginBlob_ || mgr->loginBlob_->authData.empty()) {
                    ESP_LOGE(TAG, "LoginBlob became invalid during processing");
                    return ESP_OK;
                }
                deferAuthenticationTask(mgr);
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

    std::map<std::string, std::string> SpotifyAuthManager::parseFormData(const std::string &content) {
        std::map<std::string, std::string> queryMap;
        size_t start = 0;
        while (start < content.length()) {
            size_t amp_pos = content.find('&', start);
            if (amp_pos == std::string::npos) {
                amp_pos = content.length();
            }
            std::string param = content.substr(start, amp_pos - start);
            size_t eq_pos = param.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = param.substr(0, eq_pos);
                std::string value = urlDecode(param.substr(eq_pos + 1));
                queryMap[key] = value;
            }
            start = amp_pos + 1;
        }
        return queryMap;
    }

    bool SpotifyAuthManager::validateAuthParams(const std::map<std::string, std::string> &queryMap) {
        return queryMap.find("blob") != queryMap.end() &&
               queryMap.find("clientKey") != queryMap.end() &&
               queryMap.find("userName") != queryMap.end();
    }

    void SpotifyAuthManager::sendAuthSuccessResponse(httpd_req_t *req) {
        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "status", 101);
        cJSON_AddNumberToObject(response, "spotifyError", 0);
        cJSON_AddStringToObject(response, "statusString", "ERROR-OK");
        char *response_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response_str, strlen(response_str));
        free(response_str);
        cJSON_Delete(response);
    }

    void SpotifyAuthManager::deferAuthenticationTask(SpotifyAuthManager *mgr) {
        xTaskCreate([](void *param) {
            auto *mgr = static_cast<SpotifyAuthManager *>(param);
            esp_err_t auth_result = mgr->onSpotifyConnected(mgr->loginBlob_);
            if (auth_result != ESP_OK) {
                ESP_LOGW(TAG, "Authentication processing failed");
            } else if (mgr->authenticatedCallback_) {
                mgr->authenticatedCallback_();
            }
            vTaskDelete(NULL);
        }, "spotify_auth", 16384, mgr, 5, NULL);
    }

    esp_err_t SpotifyAuthManager::closeHandler(httpd_req_t *req) {
        ESP_LOGI(TAG, "GET /close request received");
        httpd_resp_send(req, "", 0);
        return ESP_OK;
    }

    SpotifyAuthManager *SpotifyAuthManager::getInstanceFromRequest(httpd_req_t *req) {
        return static_cast<SpotifyAuthManager *>(req->user_ctx);
    }

    esp_err_t SpotifyAuthManager::onSpotifyConnected(const std::shared_ptr<cspot::LoginBlob> &blob) {
        if (!blob) {
            ESP_LOGE(TAG, "Invalid login blob received");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Spotify app connected! Processing authentication...");

        // Create a copy of the current stored blob for comparison (if it exists)
        std::shared_ptr<cspot::LoginBlob> previousBlob = nullptr;
        if (loginBlob_ && isAuthenticated_) {
            ESP_LOGI(TAG, "Found existing stored auth blob, comparing with incoming...");
            previousBlob = loginBlob_;
            compareAuthBlobs(previousBlob, blob);
        } else {
            ESP_LOGI(TAG, "No existing stored auth blob found, saving new auth data");
        }

        loginBlob_ = blob;
        storedUsername_ = blob->getUserName();
        isAuthenticated_ = true;
        saveAuthBlob();
        ESP_LOGI(TAG, "Auth data: %s", loginBlob_->toJson().c_str());
        ESP_LOGI(TAG, "Spotify authentication complete for user: %s", storedUsername_.c_str());
        stopHttpServer();
        stopZeroconfDiscovery();
        ESP_LOGI(TAG, "Cleaned up HTTP server and mDNS after authentication");
        return ESP_OK;
    }

    std::string SpotifyAuthManager::urlDecode(const std::string &encoded) {
        std::string decoded;
        decoded.reserve(encoded.length());
        for (size_t i = 0; i < encoded.length(); ++i) {
            if (encoded[i] == '%') {
                if (i + 2 < encoded.length()) {
                    std::string hex = encoded.substr(i + 1, 2);
                    try {
                        int charValue = std::stoi(hex, nullptr, 16);
                        decoded += static_cast<char>(charValue);
                        i += 2;
                    } catch (...) {
                        decoded += '%';
                    }
                } else {
                    decoded += '%';
                }
            } else if (encoded[i] == '+') {
                decoded += ' ';
            } else {
                decoded += encoded[i];
            }
        }
        return decoded;
    }

    bool SpotifyAuthManager::isStoredAuthValid() const {
        if (!loginBlob_ || !isAuthenticated_) {
            return false;
        }
        
        // Check if critical auth data exists
        if (loginBlob_->authData.empty() || loginBlob_->getUserName().empty()) {
            ESP_LOGW(TAG, "Stored auth blob missing critical data");
            return false;
        }
        
        // Basic validation of auth data structure
        if (loginBlob_->authData.size() < 10) { // Minimum reasonable size
            ESP_LOGW(TAG, "Stored auth data too small to be valid");
            return false;
        }
        
        ESP_LOGD(TAG, "Stored auth validation passed for user: %s", loginBlob_->getUserName().c_str());
        return true;
    }

    void SpotifyAuthManager::compareAuthBlobs(const std::shared_ptr<cspot::LoginBlob> &storedBlob, 
                                             const std::shared_ptr<cspot::LoginBlob> &incomingBlob) const {
        ESP_LOGI(TAG, "🔍 DEBUGGING: Comparing stored vs incoming auth blobs");
        
        if (!storedBlob || !incomingBlob) {
            ESP_LOGE(TAG, "One or both blobs are null - stored: %s, incoming: %s", 
                     storedBlob ? "valid" : "null", incomingBlob ? "valid" : "null");
            return;
        }
        
        // Compare basic fields
        ESP_LOGI(TAG, "Stored username: '%s' | Incoming username: '%s'", 
                 storedBlob->getUserName().c_str(), incomingBlob->getUserName().c_str());
        ESP_LOGI(TAG, "Stored device ID: '%s' | Incoming device ID: '%s'", 
                 storedBlob->getDeviceId().c_str(), incomingBlob->getDeviceId().c_str());
        ESP_LOGI(TAG, "Stored device name: '%s' | Incoming device name: '%s'", 
                 storedBlob->getDeviceName().c_str(), incomingBlob->getDeviceName().c_str());
        ESP_LOGI(TAG, "Stored auth type: %d | Incoming auth type: %d", 
                 storedBlob->authType, incomingBlob->authType);
        
        // Compare auth data sizes
        ESP_LOGI(TAG, "Stored auth data size: %d bytes | Incoming auth data size: %d bytes", 
                 storedBlob->authData.size(), incomingBlob->authData.size());
        
        // Check if auth data is identical
        bool authDataIdentical = (storedBlob->authData.size() == incomingBlob->authData.size()) &&
                                std::equal(storedBlob->authData.begin(), storedBlob->authData.end(), 
                                          incomingBlob->authData.begin());
        
        ESP_LOGI(TAG, "Auth data identical: %s", authDataIdentical ? "YES" : "NO");
        
        if (!authDataIdentical && storedBlob->authData.size() > 0 && incomingBlob->authData.size() > 0) {
            // Show first few bytes for debugging
            ESP_LOGI(TAG, "First 16 bytes of stored auth data:");
            for (int i = 0; i < std::min(16, (int)storedBlob->authData.size()); i++) {
                ESP_LOGI(TAG, "  [%d]: 0x%02X (%d)", i, storedBlob->authData[i], storedBlob->authData[i]);
            }
            
            ESP_LOGI(TAG, "First 16 bytes of incoming auth data:");
            for (int i = 0; i < std::min(16, (int)incomingBlob->authData.size()); i++) {
                ESP_LOGI(TAG, "  [%d]: 0x%02X (%d)", i, incomingBlob->authData[i], incomingBlob->authData[i]);
            }
        }
        
        // Compare JSON representations
        try {
            std::string storedJson = storedBlob->toJson();
            std::string incomingJson = incomingBlob->toJson();
            
            ESP_LOGI(TAG, "Stored JSON: %s", storedJson.c_str());
            ESP_LOGI(TAG, "Incoming JSON: %s", incomingJson.c_str());
            
            bool jsonIdentical = (storedJson == incomingJson);
            ESP_LOGI(TAG, "JSON representations identical: %s", jsonIdentical ? "YES" : "NO");
            
                 } catch (const std::exception& e) {
             ESP_LOGE(TAG, "Failed to serialize blobs to JSON for comparison: %s", e.what());
         }
     }

    esp_err_t SpotifyAuthManager::debugCompareWithFreshAuth() {
        if (!isAuthenticated_ || !loginBlob_) {
            ESP_LOGW(TAG, "No stored authentication available for comparison");
            return ESP_FAIL;
        }
        
        ESP_LOGI(TAG, "🔧 DEBUG: Starting fresh authentication to compare with stored auth");
        ESP_LOGI(TAG, "This will trigger Zeroconf discovery - connect from your Spotify app");
        
        // Store the current auth blob for comparison
        auto storedBlob = loginBlob_;
        
        // Clear current auth and start fresh discovery
        bool wasAuthenticated = isAuthenticated_;
        isAuthenticated_ = false;
        loginBlob_.reset();
        
        // Start zeroconf to get fresh auth
        esp_err_t ret = startZeroconfDiscovery();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start zeroconf for debug comparison");
            // Restore previous state
            loginBlob_ = storedBlob;
            isAuthenticated_ = wasAuthenticated;
            return ret;
        }
        
        ESP_LOGI(TAG, "Zeroconf started for debug comparison. Connect from Spotify app to see differences.");
        ESP_LOGI(TAG, "The comparison will be logged when you connect.");
        
        return ESP_OK;
    }

} // namespace btspeaker 