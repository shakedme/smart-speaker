#include "../include/spotify_pipeline.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
}

#include "CSpotContext.h"
#include "LoginBlob.h"
#include "DeviceStateHandler.h"
#include "BellTask.h"
#include "WrappedSemaphore.h"
#include "PCM5102AudioSink.h"
#include "../../cspot/esp32/main/EspPlayer.h"

static const char* TAG = "SpotifyPipeline";

namespace btspeaker {

SpotifyPipeline::SpotifyPipeline()
    : is_playing_(false)
    , is_initialized_(false)
    , is_running_(false)
    , volume_(100)
    , is_authenticated_(false) {
}

SpotifyPipeline::~SpotifyPipeline() {
    stop();
}

esp_err_t SpotifyPipeline::initialize() {
    if (is_initialized_) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Spotify pipeline with persistent authentication");
    
    // Initialize NVS for auth storage
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs to be erased and reinitialized");
        // We don't erase here to preserve other NVS data
    }
    
    // Initialize mDNS for Spotify discovery
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set("cspot");
    
    // Create login semaphore
    login_semaphore_ = std::make_shared<bell::WrappedSemaphore>();
    
    // Try to load existing authentication
    if (loadStoredAuth() == ESP_OK) {
        ESP_LOGI(TAG, "Loaded stored Spotify authentication for user: %s", stored_username_.c_str());
        is_authenticated_ = true;
    } else {
        ESP_LOGI(TAG, "No stored authentication found. Will start Zeroconf discovery.");
        // Create fresh login blob for pairing
        login_blob_ = std::make_shared<cspot::LoginBlob>("ESP32_SPEAKER");
        is_authenticated_ = false;
    }
    
    is_initialized_ = true;
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
        login_blob_ = std::make_shared<cspot::LoginBlob>("ESP32_SPEAKER");
        login_blob_->loadJson(std::string(auth_json.data()));
        stored_username_ = login_blob_->getUserName();
        
        ESP_LOGI(TAG, "Successfully loaded stored authentication for: %s", stored_username_.c_str());
        return ESP_OK;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to parse stored authentication: %s", e.what());
        return ESP_FAIL;
    }
}

esp_err_t SpotifyPipeline::saveAuthBlob() {
    if (!login_blob_ || login_blob_->authData.empty()) {
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
        std::string auth_json = login_blob_->toJson();
        err = nvs_set_blob(nvs_handle, NVS_AUTH_KEY, auth_json.c_str(), auth_json.length());
        
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Successfully saved authentication for user: %s", login_blob_->getUserName().c_str());
                stored_username_ = login_blob_->getUserName();
            }
        }
        
    } catch (const std::exception& e) {
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
    
    is_authenticated_ = false;
    stored_username_ = "";
    login_blob_.reset();
}

esp_err_t SpotifyPipeline::startZeroconfDiscovery() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!isWifiConnected()) {
        ESP_LOGE(TAG, "WiFi not connected - cannot start Zeroconf discovery");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting Spotify Connect Zeroconf discovery...");
    ESP_LOGI(TAG, "Open Spotify app and look for 'ESP32_SPEAKER' in Connect devices");
    
    // Create fresh login blob for pairing
    login_blob_ = std::make_shared<cspot::LoginBlob>("ESP32_SPEAKER");
    
    // Set up mDNS service for Spotify discovery
    mdns_service_add(NULL, "_spotify-connect", "_tcp", 4370, NULL, 0);
    
    // Add required TXT records for Spotify Connect
    mdns_txt_item_t spotify_txt[] = {
        {"VERSION", "1.0"},
        {"CPath", "/"},
        {"Stack", "SP"},
    };
    mdns_service_txt_set("_spotify-connect", "_tcp", spotify_txt, 3);
    
    ESP_LOGI(TAG, "Zeroconf discovery started. Device visible as 'ESP32_SPEAKER'");
    ESP_LOGI(TAG, "Device ID: %s", login_blob_->getDeviceId().c_str());
    
    return ESP_OK;
}

// Check if WiFi is connected (assumes WiFi is handled externally)
bool SpotifyPipeline::isWifiConnected() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }
    
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    return (ret == ESP_OK && ip_info.ip.addr != 0);
}

esp_err_t SpotifyPipeline::start() {
    if (!is_initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (is_running_) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting Spotify pipeline");
    
    // If we have stored authentication, try to use it
    if (is_authenticated_ && login_blob_) {
        ESP_LOGI(TAG, "Using stored authentication for user: %s", stored_username_.c_str());
        esp_err_t ret = startAuthentication();
        if (ret == ESP_OK) {
            is_running_ = true;
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Stored authentication failed, clearing and starting discovery");
            clearStoredAuth();
        }
    }
    
    // No authentication or authentication failed - start Zeroconf discovery
    if (!is_authenticated_ || !login_blob_) {
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
        is_running_ = true;
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_STATE;
}

esp_err_t SpotifyPipeline::startAuthentication() {
    if (!login_blob_) {
        ESP_LOGE(TAG, "No login blob available");
        return ESP_ERR_INVALID_STATE;
    }
    
    try {
        // Create Spotify context with our stored credentials
        context_ = cspot::Context::createFromBlob(login_blob_);
        
        if (!context_) {
            ESP_LOGE(TAG, "Failed to create Spotify context from blob");
            return ESP_FAIL;
        }
        
        // Set up device state handler with disconnect callback
        device_handler_ = std::make_shared<cspot::DeviceStateHandler>(context_, [this]() {
            ESP_LOGI(TAG, "Spotify session disconnected");
            is_running_ = false;
        });
        
        // Set up command callback
        device_handler_->stateCallback = [this](cspot::DeviceStateHandler::Command command) {
            handleSpotifyCommand(command);
        };
        
        ESP_LOGI(TAG, "Successfully authenticated with Spotify");
        is_authenticated_ = true;
        return ESP_OK;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Authentication failed: %s", e.what());
        return ESP_FAIL;
    }
}

void SpotifyPipeline::handleSpotifyCommand(const cspot::DeviceStateHandler::Command& command) {
    ESP_LOGI(TAG, "Received Spotify command: %d", static_cast<int>(command.commandType));
    
    switch (command.commandType) {
        case cspot::DeviceStateHandler::CommandType::PLAY:
            ESP_LOGI(TAG, "Play command received");
            is_playing_ = true;
            break;
            
        case cspot::DeviceStateHandler::CommandType::PAUSE:
            ESP_LOGI(TAG, "Pause command received");
            is_playing_ = false;
            break;
            
        case cspot::DeviceStateHandler::CommandType::STOP:
            ESP_LOGI(TAG, "Stop command received");
            is_playing_ = false;
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
                volume_ = static_cast<uint8_t>(std::get<int32_t>(command.data));
                ESP_LOGI(TAG, "Volume set to: %d", volume_);
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
            is_playing_ = true;
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
            is_running_ = false;
            break;
            
        case cspot::DeviceStateHandler::CommandType::SET_SHUFFLE:
            ESP_LOGI(TAG, "Set shuffle command received");
            break;
            
        case cspot::DeviceStateHandler::CommandType::SET_REPEAT:
            ESP_LOGI(TAG, "Set repeat command received");
            break;
            
        default:
            ESP_LOGD(TAG, "Unhandled Spotify command: %d", static_cast<int>(command.commandType));
            break;
    }
}

esp_err_t SpotifyPipeline::stop() {
    if (!is_initialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping Spotify pipeline");
    is_running_ = false;
    is_playing_ = false;
    if (device_handler_) {
        device_handler_->disconnect();
    }
    return ESP_OK;
}

esp_err_t SpotifyPipeline::pause() {
    if (!is_initialized_ || !is_playing_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Pausing Spotify playback");
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::PAUSE);
    }
    return ESP_OK;
}

esp_err_t SpotifyPipeline::resume() {
    if (!is_initialized_ || is_playing_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Resuming Spotify playback");
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
    }
    return ESP_OK;
}

esp_err_t SpotifyPipeline::play(const std::string& uri) {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    return playUriInternal(uri);
}

esp_err_t SpotifyPipeline::playUriInternal(const std::string& uri) {
    if (!device_handler_ || !context_) {
        ESP_LOGE(TAG, "Device handler or context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "[playUriInternal] Playing Spotify URI: %s", uri.c_str());

    // Clear current queue
    device_handler_->currentTracks.clear();

    // Create ProvidedTrack for the URI
    ProvidedTrack track = ProvidedTrack_init_zero;
    track.uri = strdup(uri.c_str());
    track.provider = strdup("queue");
    device_handler_->currentTracks.push_back(track);
    device_handler_->offset = 0;

    // Reset preloaded tracks
    if (device_handler_->trackQueue) {
        device_handler_->trackQueue->preloadedTracks.clear();
    }

    // Trigger playback
    device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
    return ESP_OK;
}

esp_err_t SpotifyPipeline::search(const std::string& query) {
    ESP_LOGI(TAG, "Searching Spotify: %s", query.c_str());
    // Search functionality would need to be implemented via the Context/Session
    // For now, just log the search request
    return ESP_OK;
}

esp_err_t SpotifyPipeline::skipNext() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Skipping to next track");
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::SKIP_NEXT);
    }
    return ESP_OK;
}

esp_err_t SpotifyPipeline::skipPrevious() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Skipping to previous track");
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::SKIP_PREV);
    }
    return ESP_OK;
}

void SpotifyPipeline::setVolume(uint8_t volume) {
    volume_ = volume;
    ESP_LOGI(TAG, "Setting volume to: %d", volume_);
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::VOLUME, static_cast<int32_t>(volume));
    }
}

// This is called when the Spotify app connects to our device via Zeroconf
esp_err_t SpotifyPipeline::onSpotifyConnected(std::shared_ptr<cspot::LoginBlob> blob) {
    if (!blob) {
        ESP_LOGE(TAG, "Invalid login blob received");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Spotify app connected! Saving authentication...");
    
    login_blob_ = blob;
    stored_username_ = blob->getUserName();
    
    // Save the authentication blob to NVS for future use
    esp_err_t ret = saveAuthBlob();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Authentication saved successfully for user: %s", stored_username_.c_str());
        is_authenticated_ = true;
        
        // Now start the actual Spotify session
        ret = startAuthentication();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Spotify session started successfully");
        } else {
            ESP_LOGE(TAG, "Failed to start Spotify session: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Failed to save authentication: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

} // namespace btspeaker 