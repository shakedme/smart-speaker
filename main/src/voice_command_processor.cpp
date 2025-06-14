#include "../include/voice_command_processor.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/audio_source_manager.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include "cJSON.h"
}

namespace btspeaker {

// Music command keywords for offline processing
const char* VoiceCommandProcessor::MUSIC_COMMANDS[] = {
    "play", "pause", "stop", "resume", "next", "previous", 
    "volume up", "volume down", "skip", "start over"
};

const size_t VoiceCommandProcessor::MUSIC_COMMANDS_COUNT = 
    sizeof(MUSIC_COMMANDS) / sizeof(MUSIC_COMMANDS[0]);

VoiceCommandProcessor::VoiceCommandProcessor()
    : state_(State::IDLE)
    , state_mutex_(nullptr)
    , websocket_client_(nullptr)
    , server_available_(false)
{
    ESP_LOGI(TAG, "VoiceCommandProcessor created");
}

VoiceCommandProcessor::~VoiceCommandProcessor() {
    shutdown();
}

esp_err_t VoiceCommandProcessor::init(WebSocketVoiceClient* websocket_client) {
    if (!websocket_client) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    websocket_client_ = websocket_client;
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Set up WebSocket callbacks
    auto audio_callback = [this](const uint8_t* audio_data, size_t length, bool is_final, const char* intent_json) {
        handle_websocket_response(audio_data, length, is_final, intent_json);
    };
    websocket_client_->set_audio_response_callback(audio_callback);
    
    auto state_callback = [this](WebSocketVoiceClient::ConnectionState new_state) {
        handle_connection_state_change(static_cast<int>(new_state));
    };
    websocket_client_->set_state_change_callback(state_callback);
    
    ESP_LOGI(TAG, "Voice command processor initialized");
    return ESP_OK;
}

void VoiceCommandProcessor::shutdown() {
    ESP_LOGI(TAG, "Shutting down voice command processor");
    
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    
    state_.store(State::IDLE);
}

esp_err_t VoiceCommandProcessor::cancel_recording() {
    ESP_LOGI(TAG, "Cancelling voice command processing");
    
    if (server_available_ && websocket_client_ && websocket_client_->is_connected()) {
        websocket_client_->cancel_operations();
    }
    
    set_state(State::IDLE);
    return ESP_OK;
}

void VoiceCommandProcessor::set_music_playback_callback(MusicPlaybackCallback callback) {
    music_callback_ = callback;
}

bool VoiceCommandProcessor::check_music_command(const char* text) {
    if (!text) {
        return false;
    }
    
    // Simple keyword matching for music commands
    for (size_t i = 0; i < MUSIC_COMMANDS_COUNT; i++) {
        if (strstr(text, MUSIC_COMMANDS[i]) != nullptr) {
            ESP_LOGI(TAG, "Music command detected: %s", MUSIC_COMMANDS[i]);
            if (music_callback_) {
                music_callback_(MUSIC_COMMANDS[i]);
            }
            return true;
        }
    }
    
    return false;
}

void VoiceCommandProcessor::handle_websocket_response(const uint8_t* audio_data, size_t length, bool is_final, const char* message_json) {
    // Handle binary audio data (voice response streaming)
    if (audio_data && length > 0) {
        ESP_LOGI(TAG, "Received TTS audio from backend (%zu bytes)", length);
        set_state(State::PLAYING_RESPONSE);
        // Queue audio chunk for streaming playback via AudioSourceManager
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        audio_source_mgr.queueVoiceResponseChunk(audio_data, length, false);
        return;
    }

    // Handle text messages (state changes and intents)
    if (message_json && strlen(message_json) > 0) {
        cJSON* root = cJSON_Parse(message_json);
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse message JSON");
            return;
        }

        // Check for state change messages
        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        if (type_item && cJSON_IsString(type_item)) {
            std::string msg_type = type_item->valuestring;
            
            if (msg_type == "state_change") {
                cJSON* state_item = cJSON_GetObjectItem(root, "state");
                if (state_item && cJSON_IsString(state_item)) {
                    handle_backend_state_change(state_item->valuestring);
                }
                cJSON_Delete(root);
                return;
            }
        }

        // Handle intent messages (is_final indicates this is an intent, not a state change)
        if (is_final) {
            ESP_LOGI(TAG, "Received intent JSON from backend: %s", message_json);
            set_state(State::PROCESSING);
            
            cJSON* intent_item = cJSON_GetObjectItem(root, "intent");
            if (intent_item && cJSON_IsString(intent_item)) {
                std::string intent_type = intent_item->valuestring;
                cJSON* data_item = cJSON_GetObjectItem(root, "data");
                std::string data_value = (data_item && cJSON_IsString(data_item)) ? data_item->valuestring : "";

                if (intent_type == "spotify") {
                    ESP_LOGI(TAG, "Handling Spotify intent with data: %s", data_value.c_str());
                    if (music_callback_ && !data_value.empty()) {
                        music_callback_(data_value.c_str());
                    }
                    set_state(State::IDLE);
                } else if (intent_type == "playback_control") {
                    ESP_LOGI(TAG, "Handling playback control intent: %s", data_value.c_str());
                    if (music_callback_ && !data_value.empty()) {
                        music_callback_(data_value.c_str());
                    }
                    set_state(State::IDLE);
                } else if (intent_type == "information") {
                    ESP_LOGI(TAG, "Information intent acknowledged. TTS audio will stream.");
                    // State will be managed by backend state changes
                } else {
                    ESP_LOGW(TAG, "Unknown intent type: %s", intent_type.c_str());
                    set_state(State::IDLE);
                }
            }
        }
        cJSON_Delete(root);
    }
}

void VoiceCommandProcessor::handle_backend_state_change(const std::string& backend_state) {
    ESP_LOGI(TAG, "Backend state changed to: %s", backend_state.c_str());
    
    if (backend_state == "LISTENING") {
        ESP_LOGI(TAG, "Backend is listening for commands");
        set_state(State::IDLE);
        // Stop any voice response playback
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        audio_source_mgr.stopVoiceResponse();
    } else if (backend_state == "PROCESSING_INTENT") {
        ESP_LOGI(TAG, "Backend is processing intent");
        set_state(State::PROCESSING);
    } else if (backend_state == "STREAMING_RESPONSE") {
        ESP_LOGI(TAG, "Backend is streaming voice response");
        set_state(State::PLAYING_RESPONSE);
    } else if (backend_state == "WAITING_FOR_USER") {
        ESP_LOGI(TAG, "Backend is waiting for user response (conversation mode)");
        // Stop voice response playback but keep session active
        auto& audio_source_mgr = AudioSourceManager::getInstance();
        audio_source_mgr.queueVoiceResponseChunk(nullptr, 0, true);
        set_state(State::IDLE);
    } else if (backend_state == "HANDLING_COMMAND") {
        ESP_LOGI(TAG, "Backend is handling command");
        set_state(State::PROCESSING);
    } else {
        ESP_LOGW(TAG, "Unknown backend state: %s", backend_state.c_str());
    }
}

void VoiceCommandProcessor::handle_connection_state_change(int connection_state) {
    auto ws_state = static_cast<WebSocketVoiceClient::ConnectionState>(connection_state);
    
    switch (ws_state) {
        case WebSocketVoiceClient::ConnectionState::CONNECTED:
            ESP_LOGI(TAG, "WebSocket server connected - online mode available");
            server_available_ = true;
            set_state(State::IDLE);
            break;
            
        case WebSocketVoiceClient::ConnectionState::CONNECTING:
            ESP_LOGI(TAG, "WebSocket connecting...");
            server_available_ = false;
            break;
            
        case WebSocketVoiceClient::ConnectionState::DISCONNECTED:
        {
            ESP_LOGW(TAG, "WebSocket server disconnected - going idle");
            server_available_ = false;
            // Finalize any audio that was playing
            auto& audio_source_mgr = AudioSourceManager::getInstance();
            audio_source_mgr.stopVoiceResponse();
            set_state(State::IDLE);
            break;
        }

        case WebSocketVoiceClient::ConnectionState::ERROR:
        {
            ESP_LOGW(TAG, "WebSocket server unavailable - using offline mode");
            server_available_ = false;
            auto& audio_source_mgr = AudioSourceManager::getInstance();
            audio_source_mgr.stopVoiceResponse();
            set_state(State::IDLE);
            break;
        }
    }
}

void VoiceCommandProcessor::set_state(State new_state) {
    if (!state_mutex_) {
        state_.store(new_state);
        return;
    }
    
    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        State old_state = state_.load();
        state_.store(new_state);
        
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        }
        
        xSemaphoreGive(state_mutex_);
    } else {
        ESP_LOGW(TAG, "Failed to acquire state mutex, setting state directly");
        state_.store(new_state);
    }
}

} // namespace btspeaker 