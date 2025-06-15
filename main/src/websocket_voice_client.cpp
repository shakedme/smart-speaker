#include "../include/websocket_voice_client.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
}

#include <utility>

namespace btspeaker {

WebSocketVoiceClient::WebSocketVoiceClient(const Config& config)
    : config_(config)
    , state_(ConnectionState::DISCONNECTED)
    , websocket_client_(nullptr)
    , state_mutex_(nullptr)
    , reconnect_task_handle_(nullptr)
    , initialized_(false)
    , recording_session_active_(false)
    , reconnect_attempts_(0)
{
    ESP_LOGI(TAG, "WebSocketVoiceClient created");
}

WebSocketVoiceClient::~WebSocketVoiceClient() {
    shutdown();
}

esp_err_t WebSocketVoiceClient::init() {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "WebSocket voice client initialized");
    return ESP_OK;
}

void WebSocketVoiceClient::shutdown() {
    ESP_LOGI(TAG, "Shutting down WebSocket voice client");
    
    disconnect();
    
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    
    initialized_ = false;
}

esp_err_t WebSocketVoiceClient::connect() {
    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (state_.load() == ConnectionState::CONNECTED) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Connecting to WebSocket server: %s", config_.server_url.c_str());
    set_state(ConnectionState::CONNECTING);
    
    esp_err_t ret = create_websocket_client();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WebSocket client: %s", esp_err_to_name(ret));
        set_state(ConnectionState::ERROR);
        return ret;
    }
    
    ret = esp_websocket_client_start(websocket_client_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
        destroy_websocket_client();
        set_state(ConnectionState::ERROR);
        return ret;
    }
    
    return ESP_OK;
}

void WebSocketVoiceClient::disconnect() {
    if (websocket_client_) {
        esp_websocket_client_stop(websocket_client_);
        destroy_websocket_client();
    }
    
    // Stop reconnection task if running
    if (reconnect_task_handle_) {
        vTaskDelete(reconnect_task_handle_);
        reconnect_task_handle_ = nullptr;
    }
    
    recording_session_active_ = false;
    set_state(ConnectionState::DISCONNECTED);
}

esp_err_t WebSocketVoiceClient::start_recording_session() {
    if (!is_connected()) {
        ESP_LOGE(TAG, "Not connected to server, cannot start recording session");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (recording_session_active_) {
        ESP_LOGW(TAG, "Recording session is already active");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting audio recording session to backend");
    recording_session_active_ = true;
    return ESP_OK;
}

esp_err_t WebSocketVoiceClient::send_audio_chunk(const uint8_t* audio_data, size_t length) {
    if (!is_connected() || !recording_session_active_) {
        ESP_LOGE(TAG, "Not connected or recording session not active, cannot send audio");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!audio_data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Send binary frame with audio data
    esp_err_t ret = esp_websocket_client_send_bin(websocket_client_, 
                                                  reinterpret_cast<const char*>(audio_data), 
                                                  length, 
                                                  portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send audio chunk: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Sent audio chunk: %zu bytes", length);
    return ESP_OK;
}

esp_err_t WebSocketVoiceClient::stop_recording_session() {
    if (!recording_session_active_) {
        ESP_LOGW(TAG, "Recording session not currently active");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping audio recording session");
    recording_session_active_ = false;
    
    // The connection is not closed here. The server will close it when it's done.
    // Or the main manager can call disconnect().
    
    return ESP_OK;
}

void WebSocketVoiceClient::set_audio_response_callback(AudioResponseCallback callback) {
    audio_callback_ = std::move(callback);
}

void WebSocketVoiceClient::set_state_change_callback(StateChangeCallback callback) {
    state_callback_ = std::move(callback);
}

void WebSocketVoiceClient::cancel_operations() {
    recording_session_active_ = false;
    
    // Disconnect will trigger cleanup.
    disconnect();
}

// Static WebSocket event handler
void WebSocketVoiceClient::websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                                   int32_t event_id, void* event_data) {
    auto* client = static_cast<WebSocketVoiceClient*>(handler_args);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
    
    client->handle_websocket_event(static_cast<esp_websocket_event_id_t>(event_id), data);
}

void WebSocketVoiceClient::handle_websocket_event(esp_websocket_event_id_t event_id, 
                                                  esp_websocket_event_data_t* event_data) {
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            reconnect_attempts_ = 0;
            set_state(ConnectionState::CONNECTED);
            
            // Automatically start recording session once connected
            start_recording_session();
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            recording_session_active_ = false;
            set_state(ConnectionState::DISCONNECTED);
            
            // Start reconnection task if configured
            if (reconnect_attempts_ < config_.max_reconnect_attempts) {
                xTaskCreate(reconnect_task, "ws_reconnect", 4096, this, 5, &reconnect_task_handle_);
            } else {
                ESP_LOGE(TAG, "Max reconnection attempts reached");
                set_state(ConnectionState::ERROR);
            }
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (event_data->op_code == 0x1) { // Text frame
                handle_text_message(event_data->data_ptr, event_data->data_len);
            } else if (event_data->op_code == 0x2) { // Binary frame
                handle_binary_message(reinterpret_cast<const uint8_t*>(event_data->data_ptr), 
                                    event_data->data_len);
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            set_state(ConnectionState::ERROR);
            break;
            
        default:
            break;
    }
}

esp_err_t WebSocketVoiceClient::create_websocket_client() {
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = config_.server_url.c_str();
    websocket_cfg.keep_alive_idle = config_.keepalive_interval_s;
    websocket_cfg.ping_interval_sec = config_.ping_interval_s;
    
    websocket_client_ = esp_websocket_client_init(&websocket_cfg);
    if (!websocket_client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }
    
    esp_err_t ret = esp_websocket_register_events(websocket_client_, 
                                                 WEBSOCKET_EVENT_ANY, 
                                                 websocket_event_handler, 
                                                 this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket events");
        esp_websocket_client_destroy(websocket_client_);
        websocket_client_ = nullptr;
        return ret;
    }
    
    return ESP_OK;
}

void WebSocketVoiceClient::destroy_websocket_client() {
    if (websocket_client_) {
        esp_websocket_client_destroy(websocket_client_);
        websocket_client_ = nullptr;
    }
}

void WebSocketVoiceClient::set_state(ConnectionState new_state) {
    ConnectionState old_state = state_.exchange(new_state);
    
    if (old_state != new_state) {
        ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        
        if (state_callback_) {
            state_callback_(new_state);
        }
    }
}

esp_err_t WebSocketVoiceClient::send_control_message(const std::string& message) {
    if (!websocket_client_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_websocket_client_send_text(websocket_client_, 
                                                   message.c_str(), 
                                                   message.length(), 
                                                   portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send control message: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Sent control message: %s", message.c_str());
    return ESP_OK;
}

void WebSocketVoiceClient::handle_text_message(const char* data, size_t length) {
    std::string message(data, length);
    ESP_LOGD(TAG, "Received backend message: %s", message.c_str());

    cJSON* root = cJSON_Parse(message.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse backend JSON response");
        return;
    }

    // Check message type
    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (type_item && cJSON_IsString(type_item)) {
        std::string msg_type = type_item->valuestring;
        
        if (msg_type == "state_change") {
            // Handle backend state change
            cJSON* state_item = cJSON_GetObjectItem(root, "state");
            if (state_item && cJSON_IsString(state_item)) {
                std::string new_state = state_item->valuestring;
                ESP_LOGI(TAG, "Backend state changed to: %s", new_state.c_str());
                
                if (audio_callback_) {
                    // Pass state change to command processor
                    audio_callback_(nullptr, 0, false, message.c_str());
                }
            }
        } else if (msg_type == "error") {
            // Handle error message
            cJSON* error_msg = cJSON_GetObjectItem(root, "message");
            if (error_msg && cJSON_IsString(error_msg)) {
                ESP_LOGE(TAG, "Received error from backend: %s", error_msg->valuestring);
            }
        }
    } else {
        // Check for legacy intent messages (for backward compatibility)
        cJSON* intent_item = cJSON_GetObjectItem(root, "intent");
        if (intent_item && cJSON_IsString(intent_item)) {
            ESP_LOGI(TAG, "Received intent from backend: %s", message.c_str());
            if (audio_callback_) {
                // Pass intent JSON to command processor
                audio_callback_(nullptr, 0, true, message.c_str());
            }
        }
    }

    cJSON_Delete(root);
}

void WebSocketVoiceClient::handle_binary_message(const uint8_t* data, size_t length) {
    ESP_LOGD(TAG, "Received binary message: %zu bytes", length);
    
    if (audio_callback_) {
        audio_callback_(data, length, false, nullptr);
    }
}

// Static reconnection task
void WebSocketVoiceClient::reconnect_task(void* pvParameters) {
    WebSocketVoiceClient* client = static_cast<WebSocketVoiceClient*>(pvParameters);
    
    vTaskDelay(pdMS_TO_TICKS(client->config_.reconnect_timeout_ms));
    
    ESP_LOGI(client->TAG, "Attempting reconnection (attempt %d/%d)", 
             client->reconnect_attempts_ + 1, client->config_.max_reconnect_attempts);
    
    client->reconnect_attempts_++;
    esp_err_t ret = client->attempt_reconnection();
    
    if (ret != ESP_OK) {
        ESP_LOGE(client->TAG, "Reconnection failed: %s", esp_err_to_name(ret));
    }
    
    client->reconnect_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t WebSocketVoiceClient::attempt_reconnection() {
    if (websocket_client_) {
        destroy_websocket_client();
    }
    
    return connect();
}

} // namespace btspeaker 