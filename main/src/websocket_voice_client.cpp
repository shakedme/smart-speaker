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
              isDestroying(false),
              voiceAssistantManager(voiceAssistantManager),
              initialized(false),
              isClientRunning(false),
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
            if (!reconnectTaskHandle) {
                xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
            }
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
            setConnectionState(ConnectionState::ERROR);
            return ret;
        }

        ret = esp_websocket_client_start(websocketClient);
        if (ret != ESP_OK) {
            destroyWebsocketClient();
            setConnectionState(ConnectionState::ERROR);
            return ret;
        }

        isClientRunning = true;
        ESP_LOGI(TAG, "WebSocket client connected successfully");
        return ESP_OK;
    }

    void WebSocketVoiceClient::disconnect() {
        if (websocketClient) {
            destroyWebsocketClient();
        }

        if (reconnectTaskHandle) {
            vTaskDelete(reconnectTaskHandle);
            reconnectTaskHandle = nullptr;
        }

        setConnectionState(ConnectionState::DISCONNECTED);
    }

    esp_err_t WebSocketVoiceClient::sendAudioChunk(const uint8_t *audioData, size_t length) {
        if (!isConnected()) return ESP_ERR_INVALID_STATE;
        if (!audioData || length == 0) return ESP_ERR_INVALID_ARG;
        if (!websocketClient) return ESP_ERR_INVALID_STATE;

        if (esp_websocket_client_is_connected(websocketClient)) {
            int ret = esp_websocket_client_send_bin(websocketClient, (const char *) audioData, length,
                                                    pdMS_TO_TICKS(1000));
            return (ret < 0) ? ESP_FAIL : ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    void WebSocketVoiceClient::cancelOperations() {
        disconnect();
    }

    void WebSocketVoiceClient::websocketEventHandler(void *handlerArgs, esp_event_base_t base, int32_t eventId,
                                                     void *eventData) {
        auto *client = static_cast<WebSocketVoiceClient *>(handlerArgs);
        if (eventId == WEBSOCKET_EVENT_DATA) {
            auto *data = static_cast<esp_websocket_event_data_t *>(eventData);
            client->handleWebsocketEvent(static_cast<esp_websocket_event_id_t>(eventId), data);
        } else {
            client->handleWebsocketEvent(static_cast<esp_websocket_event_id_t>(eventId), nullptr);
        }
    }

    void WebSocketVoiceClient::handleWebsocketEvent(esp_websocket_event_id_t eventId,
                                                    esp_websocket_event_data_t *eventData) {
        switch (eventId) {
            case WEBSOCKET_EVENT_CONNECTED:
                reconnectAttempts = 0;
                setConnectionState(ConnectionState::CONNECTED);
                break;

            case WEBSOCKET_EVENT_DISCONNECTED:
                setConnectionState(ConnectionState::DISCONNECTED);
                if (!reconnectTaskHandle && reconnectAttempts < config.maxReconnectAttempts) {
                    xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
                } else {
                    setConnectionState(ConnectionState::ERROR);
                }
                break;

            case WEBSOCKET_EVENT_DATA:
                if (eventData) {
                    if (eventData->op_code == 0x1) {
                        voiceAssistantManager.onTextData(reinterpret_cast<const char *>(eventData->data_ptr));
                    } else if (eventData->op_code == 0x2) {
                        voiceAssistantManager.onAudioResponse(reinterpret_cast<const uint8_t *>(eventData->data_ptr),
                                                              eventData->data_len);
                    }
                }
                break;

            case WEBSOCKET_EVENT_ERROR:
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
        if (!websocketClient) return ESP_FAIL;

        esp_err_t ret = esp_websocket_register_events(websocketClient, WEBSOCKET_EVENT_ANY, websocketEventHandler,
                                                      this);
        if (ret != ESP_OK) {
            esp_websocket_client_destroy(websocketClient);
            websocketClient = nullptr;
        }
        return ret;
    }

    void WebSocketVoiceClient::destroyWebsocketClient() {
        if (websocketClient && !isDestroying.exchange(true)) {
            if (isClientRunning) {
                esp_websocket_client_stop(websocketClient);
                vTaskDelay(pdMS_TO_TICKS(300));
                isClientRunning = false;
            }
            esp_websocket_client_destroy(websocketClient);
            websocketClient = nullptr;
            isDestroying = false;
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

        if (client->websocketClient) {
            client->destroyWebsocketClient();
        }

        ESP_LOGI(client->TAG, "Attempting reconnection (attempt %d/%d)", client->reconnectAttempts + 1,
                 client->config.maxReconnectAttempts);

        client->reconnectAttempts++;
        esp_err_t ret = client->connect();

        client->reconnectTaskHandle = nullptr;

        if (ret != ESP_OK) {
            ESP_LOGE(client->TAG, "Reconnection failed: %s", esp_err_to_name(ret));
        }

        vTaskDelete(nullptr);
    }

    esp_err_t WebSocketVoiceClient::attemptReconnection() {
        if (websocketClient) {
            destroyWebsocketClient();
        }
        return connect();
    }

} // namespace btspeaker
