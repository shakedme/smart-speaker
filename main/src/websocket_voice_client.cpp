#include "../include/websocket_voice_client.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
}


namespace btspeaker {

    WebSocketVoiceClient::WebSocketVoiceClient(VoiceAssistantManager &voiceAssistantManager)
            : config(),
              state(ConnectionState::DISCONNECTED),
              websocketClient(nullptr),
              stateMutex(nullptr),
              reconnectTaskHandle(nullptr),
              voiceAssistantManager(voiceAssistantManager),
              initialized(false),
              reconnectAttempts(0) {

        config.serverUrl = CONFIG_WEBSOCKET_SERVER_URL;
        config.keepaliveIntervalS = CONFIG_WS_KEEPALIVE_INTERVAL_S;
        config.pingIntervalS = CONFIG_WS_PING_INTERVAL_S;
        config.chunkSize = CONFIG_WS_CHUNK_SIZE;
        config.reconnectTimeoutMs = CONFIG_WS_RECONNECT_TIMEOUT_MS;
        config.maxReconnectAttempts = CONFIG_WS_MAX_RECONNECT_ATTEMPTS;
        ESP_LOGI(TAG, "WebSocketVoiceClient created");
    }

    WebSocketVoiceClient::~WebSocketVoiceClient() {
        shutdown();
    }

    void WebSocketVoiceClient::init() {
        stateMutex = xSemaphoreCreateMutex();
        if (!stateMutex) {
            ESP_LOGE(TAG, "Failed to create state mutex");
            throw std::runtime_error("Failed to initialize WebSocket client (state mutex creation failed)");
        }

        if (connect() != ESP_OK) {
            xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
        }
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
        ESP_LOGI(TAG, "Connecting to WebSocket server: %s", config.serverUrl.c_str());
        setConnectionState(ConnectionState::CONNECTING);

        esp_err_t ret = createWebsocketClient();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create WebSocket client: %s", esp_err_to_name(ret));
            setConnectionState(ConnectionState::ERROR);
            return ret;
        }

        ret = esp_websocket_client_start(websocketClient);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
            destroyWebsocketClient();
            setConnectionState(ConnectionState::ERROR);
            return ret;
        }

        ESP_LOGI(TAG, "WebSocket client connected successfully");
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

        setConnectionState(ConnectionState::DISCONNECTED);
    }

    esp_err_t WebSocketVoiceClient::sendAudioChunk(const uint8_t *audioData, size_t length) {
        if (!isConnected()) {
            ESP_LOGE(TAG, "Not connected, cannot send audio");
            return ESP_ERR_INVALID_STATE;
        }

        if (!audioData || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        if (!websocketClient) {
            ESP_LOGE(TAG, "WebSocket client is null");
            return ESP_ERR_INVALID_STATE;
        }

        // Send binary frame with audio data with shorter timeout
        int ret = esp_websocket_client_send_bin(websocketClient,
                                                      (const char *)(audioData),
                                                      length,
                                                      pdMS_TO_TICKS(1000));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to send audio chunk");
            return ESP_FAIL;
        }
        return ESP_OK;
    }


    void WebSocketVoiceClient::cancelOperations() {
        disconnect();
    }

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
                setConnectionState(ConnectionState::CONNECTED);
                break;

            case WEBSOCKET_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "WebSocket disconnected");
                setConnectionState(ConnectionState::DISCONNECTED);

                if (reconnectAttempts < config.maxReconnectAttempts) {
                    xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
                } else {
                    ESP_LOGE(TAG, "Max reconnection attempts reached");
                    setConnectionState(ConnectionState::ERROR);
                }
                break;

            case WEBSOCKET_EVENT_DATA:
                if (eventData->op_code == 0x1) { // Text frame
                    voiceAssistantManager.onTextData(reinterpret_cast<const char *>(eventData->data_ptr));
                } else if (eventData->op_code == 0x2) { // Binary frame
                    voiceAssistantManager.onAudioResponse(reinterpret_cast<const uint8_t *>(eventData->data_ptr),
                                                          eventData->data_len);
                }
                break;

            case WEBSOCKET_EVENT_ERROR:
                ESP_LOGE(TAG, "WebSocket error");
                setConnectionState(ConnectionState::ERROR);
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
        websocket_cfg.reconnect_timeout_ms = config.reconnectTimeoutMs;
        websocket_cfg.keep_alive_enable = true;
        websocket_cfg.network_timeout_ms = 10000;
        websocket_cfg.task_stack = 4096;

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

    void WebSocketVoiceClient::setConnectionState(ConnectionState newState) {
        ConnectionState old_state = state.exchange(newState);
        if (old_state != newState) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(newState));
        }
    }

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