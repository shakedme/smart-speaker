#include "../include/spotify_pipeline.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mdns.h"
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
    , volume_(100) {
}

SpotifyPipeline::~SpotifyPipeline() {
    stop();
}

esp_err_t SpotifyPipeline::initialize() {
    if (is_initialized_) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Spotify pipeline");
    
    // Create login semaphore
    login_semaphore_ = std::make_shared<bell::WrappedSemaphore>();
    
    // Create login blob
    login_blob_ = std::make_shared<cspot::LoginBlob>("ESP32_SPEAKER");
    
    is_initialized_ = true;
    return ESP_OK;
}



void SpotifyPipeline::handleSpotifyCommand(const cspot::DeviceStateHandler::Command& command) {
    ESP_LOGI(TAG, "Received Spotify command: %d", static_cast<int>(command.commandType));
    
    switch (command.commandType) {
        case cspot::DeviceStateHandler::CommandType::PLAY:
            ESP_LOGI(TAG, "Command: PLAY");
            is_playing_ = true;
            break;
            
        case cspot::DeviceStateHandler::CommandType::PAUSE:
            ESP_LOGI(TAG, "Command: PAUSE");
            is_playing_ = false;
            break;
            
        case cspot::DeviceStateHandler::CommandType::STOP:
            ESP_LOGI(TAG, "Command: STOP");
            is_playing_ = false;
            break;
            
        case cspot::DeviceStateHandler::CommandType::SKIP_NEXT:
            ESP_LOGI(TAG, "Command: SKIP_NEXT");
            break;
            
        case cspot::DeviceStateHandler::CommandType::SKIP_PREV:
            ESP_LOGI(TAG, "Command: SKIP_PREV");
            break;
            
        case cspot::DeviceStateHandler::CommandType::SEEK:
            ESP_LOGI(TAG, "Command: SEEK");
            // Command data contains seek position
            break;
            
        case cspot::DeviceStateHandler::CommandType::VOLUME:
            ESP_LOGI(TAG, "Command: VOLUME");
            // Command data contains volume level
            if (std::holds_alternative<int32_t>(command.data)) {
                volume_ = static_cast<uint8_t>(std::get<int32_t>(command.data));
                ESP_LOGI(TAG, "Volume set to: %d", volume_);
            }
            break;
            
        case cspot::DeviceStateHandler::CommandType::SET_SHUFFLE:
            ESP_LOGI(TAG, "Command: SET_SHUFFLE");
            // Command data contains shuffle state (bool)
            break;
            
        case cspot::DeviceStateHandler::CommandType::SET_REPEAT:
            ESP_LOGI(TAG, "Command: SET_REPEAT");
            // Command data contains repeat state
            break;
            
        case cspot::DeviceStateHandler::CommandType::TRACK_INFO:
            ESP_LOGI(TAG, "Command: TRACK_INFO");
            // Command data contains track information
            break;
            
        case cspot::DeviceStateHandler::CommandType::PLAYBACK_START:
            ESP_LOGI(TAG, "Command: PLAYBACK_START");
            is_playing_ = true;
            break;
            
        case cspot::DeviceStateHandler::CommandType::PLAYBACK:
            ESP_LOGI(TAG, "Command: PLAYBACK");
            break;
            
        case cspot::DeviceStateHandler::CommandType::DEPLETED:
            ESP_LOGI(TAG, "Command: DEPLETED");
            break;
            
        case cspot::DeviceStateHandler::CommandType::FLUSH:
            ESP_LOGI(TAG, "Command: FLUSH");
            break;
            
        case cspot::DeviceStateHandler::CommandType::DISC:
            ESP_LOGI(TAG, "Command: DISC (Disconnect)");
            is_running_ = false;
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown command type: %d", static_cast<int>(command.commandType));
            break;
    }
}

esp_err_t SpotifyPipeline::start() {
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting Spotify pipeline");
    is_running_ = true;
    
    // Create context and start connection
    auto ctx = cspot::Context::createFromBlob(login_blob_);
    
    try {
        ctx->session->connectWithRandomAp();
        ctx->config.authData = ctx->session->authenticate(login_blob_);
        
        if (ctx->config.authData.size() > 0) {
            // Create PCM5102A audio sink
            auto audio_sink = std::make_shared<PCM5102AudioSink>();
            audio_sink->setParams(44100, 2, 16);
            audio_sink->volumeChanged(volume_);

            // Create device handler
            device_handler_ = std::make_shared<cspot::DeviceStateHandler>(ctx, [this]() {
                is_running_ = false;
            });

            // Set up command callback
            device_handler_->stateCallback = [this](cspot::DeviceStateHandler::Command command) {
                handleSpotifyCommand(command);
            };

            // Start handling mercury messages
            ctx->session->startTask();

            // Create player with PCM5102A sink - this will create its own task
            auto player = std::make_shared<EspPlayer>(audio_sink, device_handler_);

            context_ = ctx;
            ESP_LOGI(TAG, "Spotify pipeline started successfully");
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to authenticate");
            return ESP_ERR_INVALID_RESPONSE;
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Error while connecting: %s", e.what());
        return ESP_ERR_INVALID_RESPONSE;
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
    
    ESP_LOGI(TAG, "Playing Spotify URI: %s", uri.c_str());
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::PLAY);
    }
    return ESP_OK;
}

esp_err_t SpotifyPipeline::search(const std::string& query) {
    ESP_LOGI(TAG, "Searching Spotify: %s", query.c_str());
    // Search functionality would need to be implemented via the Context/Session
    // For now, just log the search request
    return ESP_OK;
}

void SpotifyPipeline::setVolume(uint8_t volume) {
    volume_ = volume;
    ESP_LOGI(TAG, "Setting volume to: %d", volume_);
    if (device_handler_) {
        device_handler_->sendCommand(cspot::DeviceStateHandler::CommandType::VOLUME, static_cast<int32_t>(volume));
    }
}

} // namespace btspeaker 