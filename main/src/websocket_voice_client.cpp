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

    WebSocketVoiceClient::WebSocketVoiceClient(Config config)
            : config(std::move(config)), state(ConnectionState::DISCONNECTED), websocketClient(nullptr),
              stateMutex(nullptr), reconnectTaskHandle(nullptr), initialized(false), reconnectAttempts(0) {
        ESP_LOGI(TAG, "WebSocketVoiceClient created");
    }

    WebSocketVoiceClient::~WebSocketVoiceClient() {
        shutdown();
    }

    esp_err_t WebSocketVoiceClient::init() {
        if (initialized) {
            ESP_LOGW(TAG, "Already initialized");
            return ESP_OK;
        }

        // Create state mutex
        stateMutex = xSemaphoreCreateMutex();
        if (!stateMutex) {
            ESP_LOGE(TAG, "Failed to create state mutex");
            return ESP_ERR_NO_MEM;
        }

        initialized = true;
        ESP_LOGI(TAG, "WebSocket voice client initialized");
        return ESP_OK;
    }

    void WebSocketVoiceClient::shutdown() {
        ESP_LOGI(TAG, "Shutting down WebSocket voice client");

        disconnect();

        if (stateMutex) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
        }

        initialized = false;
    }

    esp_err_t WebSocketVoiceClient::connect() {
        if (!initialized) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        if (state.load() == ConnectionState::CONNECTED) {
            ESP_LOGW(TAG, "Already connected");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Connecting to WebSocket server: %s", config.serverUrl.c_str());
        setState(ConnectionState::CONNECTING);

        esp_err_t ret = createWebsocketClient();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create WebSocket client: %s", esp_err_to_name(ret));
            setState(ConnectionState::ERROR);
            return ret;
        }

        ret = esp_websocket_client_start(websocketClient);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
            destroyWebsocketClient();
            setState(ConnectionState::ERROR);
            return ret;
        }

        return ESP_OK;
    }

    void WebSocketVoiceClient::disconnect() {
        if (websocketClient) {
            esp_websocket_client_stop(websocketClient);
            destroyWebsocketClient();
        }

        // Stop reconnection task if running
        if (reconnectTaskHandle) {
            vTaskDelete(reconnectTaskHandle);
            reconnectTaskHandle = nullptr;
        }

        setState(ConnectionState::DISCONNECTED);
    }

    esp_err_t WebSocketVoiceClient::sendAudioChunk(const uint8_t *audioData, size_t length) {
        if (!isConnected()) {
            ESP_LOGE(TAG, "Not connected, cannot send audio");
            return ESP_ERR_INVALID_STATE;
        }

        if (!audioData || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        // Send binary frame with audio data
        esp_err_t ret = esp_websocket_client_send_bin(websocketClient,
                                                      reinterpret_cast<const char *>(audioData),
                                                      length,
                                                      portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send audio chunk: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGD(TAG, "Sent audio chunk: %zu bytes", length);
        return ESP_OK;
    }

    void WebSocketVoiceClient::setAudioResponseCallback(AudioResponseCallback callback) {
        audioCallback = std::move(callback);
    }

    void WebSocketVoiceClient::setStateChangeCallback(StateChangeCallback callback) {
        stateChangeCallback = std::move(callback);
    }

    void WebSocketVoiceClient::cancelOperations() {
        // Disconnect will trigger cleanup.
        disconnect();
    }

// Static WebSocket event handler
    void WebSocketVoiceClient::websocketEventHandler(void *handlerArgs, esp_event_base_t base,
                                                     int32_t eventId, void *eventData) {
        auto *client = static_cast<WebSocketVoiceClient *>(handlerArgs);
        auto *data = static_cast<esp_websocket_event_data_t *>(eventData);

        client->handleWebsocketEvent(static_cast<esp_websocket_event_id_t>(eventId), data);
    }

    void WebSocketVoiceClient::handleWebsocketEvent(esp_websocket_event_id_t eventId,
                                                    esp_websocket_event_data_t *eventData) {
        switch (eventId) {
            case WEBSOCKET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "WebSocket connected");
                reconnectAttempts = 0;
                setState(ConnectionState::CONNECTED);
                break;

            case WEBSOCKET_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "WebSocket disconnected");
                setState(ConnectionState::DISCONNECTED);

                // Start reconnection task if configured
                if (reconnectAttempts < config.maxReconnectAttempts) {
                    xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
                } else {
                    ESP_LOGE(TAG, "Max reconnection attempts reached");
                    setState(ConnectionState::ERROR);
                }
                break;

            case WEBSOCKET_EVENT_DATA:
                if (eventData->op_code == 0x1) { // Text frame
                    handleTextMessage(eventData->data_ptr, eventData->data_len);
                } else if (eventData->op_code == 0x2) { // Binary frame
                    handleBinaryMessage(reinterpret_cast<const uint8_t *>(eventData->data_ptr),
                                        eventData->data_len);
                }
                break;

            case WEBSOCKET_EVENT_ERROR:
                ESP_LOGE(TAG, "WebSocket error");
                setState(ConnectionState::ERROR);
                break;

            default:
                break;
        }
    }

    esp_err_t WebSocketVoiceClient::createWebsocketClient() {
        esp_websocket_client_config_t websocket_cfg = {};
        websocket_cfg.uri = config.serverUrl.c_str();
        websocket_cfg.keep_alive_idle = config.keepaliveIntervalS;
        websocket_cfg.ping_interval_sec = config.pingIntervalS;

        websocketClient = esp_websocket_client_init(&websocket_cfg);
        if (!websocketClient) {
            ESP_LOGE(TAG, "Failed to initialize WebSocket client");
            return ESP_FAIL;
        }

        esp_err_t ret = esp_websocket_register_events(websocketClient,
                                                      WEBSOCKET_EVENT_ANY,
                                                      websocketEventHandler,
                                                      this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WebSocket events");
            esp_websocket_client_destroy(websocketClient);
            websocketClient = nullptr;
            return ret;
        }

        return ESP_OK;
    }

    void WebSocketVoiceClient::destroyWebsocketClient() {
        if (websocketClient) {
            esp_websocket_client_destroy(websocketClient);
            websocketClient = nullptr;
        }
    }

    void WebSocketVoiceClient::setState(ConnectionState newState) {
        ConnectionState old_state = state.exchange(newState);

        if (old_state != newState) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(newState));

            if (stateChangeCallback) {
                stateChangeCallback(newState);
            }
        }
    }

    void WebSocketVoiceClient::handleTextMessage(const char *data, size_t length) {
        std::string message(data, length);
        ESP_LOGD(TAG, "Received backend message: %s", message.c_str());

        cJSON *root = cJSON_Parse(message.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse backend JSON response");
            return;
        }

        // Check message type
        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        if (type_item && cJSON_IsString(type_item)) {
            std::string msg_type = type_item->valuestring;

            if (msg_type == "state_change") {
                // Handle backend state change
                cJSON *state_item = cJSON_GetObjectItem(root, "state");
                if (state_item && cJSON_IsString(state_item)) {
                    std::string new_state = state_item->valuestring;
                    ESP_LOGI(TAG, "Backend state changed to: %s", new_state.c_str());

                    if (audioCallback) {
                        // Pass state change to command processor
                        audioCallback(nullptr, 0, false, message.c_str());
                    }
                }
            } else if (msg_type == "error") {
                // Handle error message
                cJSON *error_msg = cJSON_GetObjectItem(root, "message");
                if (error_msg && cJSON_IsString(error_msg)) {
                    ESP_LOGE(TAG, "Received error from backend: %s", error_msg->valuestring);
                }
            }
        } else {
            // Check for legacy intent messages (for backward compatibility)
            cJSON *intent_item = cJSON_GetObjectItem(root, "intent");
            if (intent_item && cJSON_IsString(intent_item)) {
                ESP_LOGI(TAG, "Received intent from backend: %s", message.c_str());
                if (audioCallback) {
                    // Pass intent JSON to command processor
                    audioCallback(nullptr, 0, true, message.c_str());
                }
            }
        }

        cJSON_Delete(root);
    }

    void WebSocketVoiceClient::handleBinaryMessage(const uint8_t *data, size_t length) {
        ESP_LOGD(TAG, "Received binary message: %zu bytes", length);

        if (audioCallback) {
            audioCallback(data, length, false, nullptr);
        }
    }

// Static reconnection task
    void WebSocketVoiceClient::reconnectTask(void *pvParameters) {
        auto *client = static_cast<WebSocketVoiceClient *>(pvParameters);

        vTaskDelay(pdMS_TO_TICKS(client->config.reconnectTimeoutMs));

        ESP_LOGI(client->TAG, "Attempting reconnection (attempt %d/%d)",
                 client->reconnectAttempts + 1, client->config.maxReconnectAttempts);

        client->reconnectAttempts++;
        esp_err_t ret = client->attemptReconnection();

        if (ret != ESP_OK) {
            ESP_LOGE(client->TAG, "Reconnection failed: %s", esp_err_to_name(ret));
        }

        client->reconnectTaskHandle = nullptr;
        vTaskDelete(nullptr);
    }

    esp_err_t WebSocketVoiceClient::attemptReconnection() {
        if (websocketClient) {
            destroyWebsocketClient();
        }

        return connect();
    }

} // namespace btspeaker 