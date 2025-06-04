#include "../include/voice_command_processor.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/audio_response_player.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
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
    , stream_buffer_pos_(0)
    , websocket_client_(nullptr)
    , response_player_(nullptr)
    , server_available_(false)
{
    ESP_LOGI(TAG, "VoiceCommandProcessor created");
}

VoiceCommandProcessor::~VoiceCommandProcessor() {
    shutdown();
}

esp_err_t VoiceCommandProcessor::init(WebSocketVoiceClient* websocket_client, AudioResponsePlayer* response_player) {
    if (!websocket_client || !response_player) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    websocket_client_ = websocket_client;
    response_player_ = response_player;
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize small streaming buffer
    stream_buffer_ = std::make_unique<uint8_t[]>(STREAM_CHUNK_SIZE);
    if (!stream_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate streaming buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // Set up WebSocket callbacks
    auto audio_callback = [this](const uint8_t* audio_data, size_t length, bool is_final) {
        handle_websocket_response(audio_data, length, is_final);
    };
    websocket_client_->set_audio_response_callback(audio_callback);
    
    auto state_callback = [this](WebSocketVoiceClient::ConnectionState new_state) {
        handle_connection_state_change(static_cast<int>(new_state));
    };
    websocket_client_->set_state_change_callback(state_callback);
    
    ESP_LOGI(TAG, "Voice command processor initialized (streaming buffer size: %zu bytes)", STREAM_CHUNK_SIZE);
    return ESP_OK;
}

void VoiceCommandProcessor::shutdown() {
    ESP_LOGI(TAG, "Shutting down voice command processor");
    
    set_state(State::IDLE);
    
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    
    stream_buffer_.reset();
    stream_buffer_pos_ = 0;
}

esp_err_t VoiceCommandProcessor::start_recording() {
    ESP_LOGI(TAG, "Starting voice command recording");
    
    if (state_.load() != State::IDLE) {
        ESP_LOGW(TAG, "Cannot start recording in current state: %d", static_cast<int>(state_.load()));
        return ESP_ERR_INVALID_STATE;
    }
    
    stream_buffer_pos_ = 0;
    set_state(State::RECORDING);
    
    // Start recording session on server if connected
    if (server_available_ && websocket_client_->is_connected()) {
        esp_err_t ret = websocket_client_->start_recording_session();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start server recording session, will use offline mode");
            server_available_ = false;
        }
    }
    
    ESP_LOGI(TAG, "Voice command recording started (server mode: %s)", 
             server_available_ ? "online" : "offline");
    return ESP_OK;
}

esp_err_t VoiceCommandProcessor::process_audio_data(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (state_.load() != State::RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // If server is available, stream directly
    if (server_available_ && websocket_client_->is_connected()) {
        return send_to_websocket(data, length);
    }
    
    // Otherwise, buffer for offline processing
    if (stream_buffer_pos_ + length >= STREAM_CHUNK_SIZE) {
        ESP_LOGW(TAG, "Stream buffer full, finishing recording");
        return finish_recording();
    }
    
    // Copy audio data to buffer
    memcpy(stream_buffer_.get() + stream_buffer_pos_, data, length);
    stream_buffer_pos_ += length;
    
    ESP_LOGD(TAG, "Buffered %zu bytes, total: %zu/%zu", length, stream_buffer_pos_, STREAM_CHUNK_SIZE);
    return ESP_OK;
}

esp_err_t VoiceCommandProcessor::finish_recording() {
    ESP_LOGI(TAG, "Finishing voice command recording");
    
    if (state_.load() != State::RECORDING) {
        ESP_LOGW(TAG, "Not currently recording");
        return ESP_ERR_INVALID_STATE;
    }
    
    set_state(State::PROCESSING);
    
    // Stop recording session on server if connected
    if (server_available_ && websocket_client_->is_connected()) {
        esp_err_t ret = websocket_client_->stop_recording_session();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop server recording session");
        }
        // Server will send response via WebSocket callback
        return ESP_OK;
    }
    
    // Handle offline command processing
    if (stream_buffer_pos_ > 0) {
        return handle_offline_command(stream_buffer_.get(), stream_buffer_pos_);
    } else {
        ESP_LOGW(TAG, "No audio data recorded");
        set_state(State::IDLE);
        return ESP_ERR_INVALID_SIZE;
    }
}

esp_err_t VoiceCommandProcessor::cancel_recording() {
    ESP_LOGI(TAG, "Cancelling voice command recording");
    
    stream_buffer_pos_ = 0;
    
    // Cancel server operations if connected
    if (server_available_ && websocket_client_->is_connected()) {
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

esp_err_t VoiceCommandProcessor::send_to_websocket(const uint8_t* audio_data, size_t length) {
    if (!websocket_client_ || !websocket_client_->is_connected()) {
        ESP_LOGE(TAG, "WebSocket client not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGD(TAG, "Streaming %zu bytes to WebSocket server", length);
    
    esp_err_t ret = websocket_client_->send_audio_chunk(audio_data, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send audio chunk: %s", esp_err_to_name(ret));
        server_available_ = false;
        return ret;
    }
    
    return ESP_OK;
}

void VoiceCommandProcessor::handle_websocket_response(const uint8_t* audio_data, size_t length, bool is_final) {
    if (is_final) {
        ESP_LOGI(TAG, "WebSocket audio response completed");
        set_state(State::IDLE);
        return;
    }
    
    if (audio_data && length > 0) {
        ESP_LOGD(TAG, "Received audio response chunk: %zu bytes", length);
        
        set_state(State::PLAYING_RESPONSE);
        
        // Queue audio for playback with is_final parameter
        if (response_player_) {
            response_player_->queue_audio_chunk(audio_data, length, is_final);
        }
    }
}

void VoiceCommandProcessor::handle_connection_state_change(int connection_state) {
    auto ws_state = static_cast<WebSocketVoiceClient::ConnectionState>(connection_state);
    
    switch (ws_state) {
        case WebSocketVoiceClient::ConnectionState::CONNECTED:
            ESP_LOGI(TAG, "WebSocket server connected - online mode available");
            server_available_ = true;
            break;
            
        case WebSocketVoiceClient::ConnectionState::DISCONNECTED:
        case WebSocketVoiceClient::ConnectionState::ERROR:
            ESP_LOGW(TAG, "WebSocket server unavailable - using offline mode");
            server_available_ = false;
            break;
            
        default:
            break;
    }
}

esp_err_t VoiceCommandProcessor::handle_offline_command(const uint8_t* audio_data, size_t length) {
    ESP_LOGI(TAG, "Processing command in offline mode (%zu bytes)", length);
    
    // In offline mode, we can only handle simple music commands
    // This is a placeholder - in a real implementation, you might:
    // 1. Use local speech recognition
    // 2. Use simple audio pattern matching
    // 3. Default to music control based on audio characteristics
    
    // For now, just assume it's a basic play command
    ESP_LOGW(TAG, "Offline mode: Assuming basic music command");
    if (music_callback_) {
        music_callback_("play");
    }
    
    set_state(State::IDLE);
    return ESP_OK;
}

void VoiceCommandProcessor::set_state(State new_state) {
    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        State old_state = state_.load();
        state_.store(new_state);
        
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        }
        
        xSemaphoreGive(state_mutex_);
    }
}

} // namespace btspeaker 