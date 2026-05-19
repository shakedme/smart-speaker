#include "../include/websocket_voice_client.hpp"
#include "../include/voice_assistant_manager.hpp"
#include "../include/voice_config.hpp"
#include <cstring>
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    namespace {

        void *allocateUplinkPayload(size_t size) {
            if (size == 0) {
                return nullptr;
            }
            void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!ptr) {
                ptr = malloc(size);
            }
            return ptr;
        }

    } // namespace

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

        if (startUplinkWorker() != ESP_OK) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
            throw std::runtime_error("Failed to start mic uplink worker");
        }

        if (connect() != ESP_OK) {
            if (!reconnectTaskHandle) {
                xTaskCreate(reconnectTask, "ws_reconnect", 4096, this, 5, &reconnectTaskHandle);
            }
        }

        initialized = true;
    }

    void WebSocketVoiceClient::shutdown() {
        ESP_LOGI(TAG, "Shutting down WebSocket voice client");

        shutdownUplinkWorker();
        disconnect();

        if (stateMutex) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
        }

        initialized = false;
    }

    esp_err_t WebSocketVoiceClient::startUplinkWorker() {
        uplinkQueue_ = xQueueCreate(UPLINK_QUEUE_DEPTH, sizeof(UplinkChunk));
        if (!uplinkQueue_) {
            ESP_LOGE(TAG, "Failed to create uplink queue");
            return ESP_FAIL;
        }

        uplinkStoppedSem_ = xSemaphoreCreateBinary();
        if (!uplinkStoppedSem_) {
            vQueueDelete(uplinkQueue_);
            uplinkQueue_ = nullptr;
            ESP_LOGE(TAG, "Failed to create uplink shutdown semaphore");
            return ESP_FAIL;
        }

        uplinkTaskStack_ = static_cast<StackType_t *>(heap_caps_malloc(
                UPLINK_TASK_STACK_WORDS * sizeof(StackType_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        uplinkTaskBuffer_ = static_cast<StaticTask_t *>(heap_caps_malloc(
                sizeof(StaticTask_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        if (!uplinkTaskStack_ || !uplinkTaskBuffer_) {
            shutdownUplinkWorker();
            ESP_LOGE(TAG, "Failed to allocate mic_uplink task stack");
            return ESP_ERR_NO_MEM;
        }

        uplinkAccepting_.store(true);
        uplinkTaskHandle_ = xTaskCreateStaticPinnedToCore(
                uplinkWorkerTask,
                "mic_uplink",
                UPLINK_TASK_STACK_WORDS,
                this,
                UPLINK_TASK_PRIORITY,
                uplinkTaskStack_,
                uplinkTaskBuffer_,
                0);

        if (!uplinkTaskHandle_) {
            shutdownUplinkWorker();
            ESP_LOGE(TAG, "Failed to create mic_uplink task");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "mic_uplink worker started (stack %u words, queue depth %u)",
                 static_cast<unsigned>(UPLINK_TASK_STACK_WORDS),
                 static_cast<unsigned>(UPLINK_QUEUE_DEPTH));
        return ESP_OK;
    }

    void WebSocketVoiceClient::freeUplinkChunk(UplinkChunk &chunk) {
        if (chunk.payload) {
            heap_caps_free(chunk.payload);
            chunk.payload = nullptr;
        }
        chunk.length = 0;
        chunk.shutdown = false;
    }

    void WebSocketVoiceClient::drainUplinkQueue() {
        if (!uplinkQueue_) {
            return;
        }

        UplinkChunk pending{};
        while (xQueueReceive(uplinkQueue_, &pending, 0) == pdTRUE) {
            if (!pending.shutdown) {
                freeUplinkChunk(pending);
            }
        }
    }

    void WebSocketVoiceClient::shutdownUplinkWorker() {
        uplinkAccepting_.store(false);

        if (uplinkQueue_) {
            UplinkChunk shutdown_chunk{
                    .shutdown = true,
                    .payload = nullptr,
                    .length = 0,
            };
            xQueueSend(uplinkQueue_, &shutdown_chunk, pdMS_TO_TICKS(100));
        }

        if (uplinkTaskHandle_ && uplinkStoppedSem_) {
            xSemaphoreTake(uplinkStoppedSem_, pdMS_TO_TICKS(5000));
            uplinkTaskHandle_ = nullptr;
        }

        drainUplinkQueue();

        if (uplinkQueue_) {
            vQueueDelete(uplinkQueue_);
            uplinkQueue_ = nullptr;
        }

        if (uplinkStoppedSem_) {
            vSemaphoreDelete(uplinkStoppedSem_);
            uplinkStoppedSem_ = nullptr;
        }

        if (uplinkTaskStack_) {
            heap_caps_free(uplinkTaskStack_);
            uplinkTaskStack_ = nullptr;
        }

        if (uplinkTaskBuffer_) {
            heap_caps_free(uplinkTaskBuffer_);
            uplinkTaskBuffer_ = nullptr;
        }
    }

    void WebSocketVoiceClient::uplinkWorkerTask(void *arg) {
        auto *self = static_cast<WebSocketVoiceClient *>(arg);
        self->runUplinkWorker();
        if (self->uplinkStoppedSem_) {
            xSemaphoreGive(self->uplinkStoppedSem_);
        }
        vTaskDelete(nullptr);
    }

    bool WebSocketVoiceClient::dropOldestUplinkChunk() {
        UplinkChunk oldest{};
        if (xQueueReceive(uplinkQueue_, &oldest, 0) != pdTRUE) {
            return false;
        }

        if (oldest.shutdown) {
            xQueueSendToFront(uplinkQueue_, &oldest, 0);
            return false;
        }

        freeUplinkChunk(oldest);
        return true;
    }

    esp_err_t WebSocketVoiceClient::enqueueUplinkChunk(UplinkChunk chunk) {
        if (!uplinkQueue_ || !uplinkAccepting_.load()) {
            freeUplinkChunk(chunk);
            return ESP_ERR_INVALID_STATE;
        }

        if (xQueueSend(uplinkQueue_, &chunk, 0) == pdTRUE) {
            uplinkDiagEnqueued_.fetch_add(1, std::memory_order_relaxed);
            return ESP_OK;
        }

        if (dropOldestUplinkChunk()) {
            uplinkDiagDroppedOldest_.fetch_add(1, std::memory_order_relaxed);
        } else {
            freeUplinkChunk(chunk);
            return ESP_FAIL;
        }

        if (xQueueSend(uplinkQueue_, &chunk, 0) == pdTRUE) {
            uplinkDiagEnqueued_.fetch_add(1, std::memory_order_relaxed);
            return ESP_OK;
        }

        freeUplinkChunk(chunk);
        return ESP_FAIL;
    }

    void WebSocketVoiceClient::logUplinkDiagnostics(const int64_t now_us,
                                                    const uint32_t enqueued,
                                                    const uint32_t sent,
                                                    const uint32_t dropped_oldest,
                                                    const uint32_t dropped_disc,
                                                    const uint32_t send_fail,
                                                    const uint32_t send_slow,
                                                    const uint64_t bytes_sent,
                                                    const uint64_t sum_send_us,
                                                    const uint32_t max_send_us,
                                                    const uint32_t queue_depth) const {
        const uint32_t avg_send_us = sent > 0
                                     ? static_cast<uint32_t>(sum_send_us / sent)
                                     : 0;
        const uint32_t effective_hz = (UPLINK_DIAG_INTERVAL_MS > 0 && bytes_sent > 0)
                                      ? static_cast<uint32_t>(
                                              (bytes_sent * 1000ULL) /
                                              (static_cast<uint64_t>(UPLINK_DIAG_INTERVAL_MS) * 2ULL))
                                      : 0;

        const char *task_name = pcTaskGetName(xTaskGetCurrentTaskHandle());
        if (task_name == nullptr) {
            task_name = "?";
        }

        ESP_LOGI(TAG,
                 "[WS_TX_DIAG] enqueued=%lu sent=%lu drop_old=%lu drop_disc=%lu send_fail=%lu slow=%lu "
                 "bytes=%lu ~%luHz send_us(avg=%lu max=%lu) q=%lu task=%s dram=%lu",
                 static_cast<unsigned long>(enqueued),
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(dropped_oldest),
                 static_cast<unsigned long>(dropped_disc),
                 static_cast<unsigned long>(send_fail),
                 static_cast<unsigned long>(send_slow),
                 static_cast<unsigned long>(bytes_sent),
                 static_cast<unsigned long>(effective_hz),
                 static_cast<unsigned long>(avg_send_us),
                 static_cast<unsigned long>(max_send_us),
                 static_cast<unsigned long>(queue_depth),
                 task_name,
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        (void) now_us;
    }

    void WebSocketVoiceClient::runUplinkWorker() {
        int64_t diag_window_start_us = esp_timer_get_time();
        uint32_t diag_sent = 0;
        uint32_t diag_dropped_disc = 0;
        uint32_t diag_send_fail = 0;
        uint32_t diag_send_slow = 0;
        uint64_t diag_bytes_sent = 0;
        uint64_t diag_sum_send_us = 0;
        uint32_t diag_max_send_us = 0;

        UplinkChunk chunk{};
        while (true) {
            if (xQueueReceive(uplinkQueue_, &chunk, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            if (chunk.shutdown) {
                break;
            }

            const uint32_t queue_depth = uplinkQueue_
                                         ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplinkQueue_))
                                         : 0;

            if (!isConnected() || !websocketClient ||
                !esp_websocket_client_is_connected(websocketClient)) {
                diag_dropped_disc++;
                freeUplinkChunk(chunk);
            } else {
                const int64_t before_send_us = esp_timer_get_time();
                const int ret = esp_websocket_client_send_bin(
                        websocketClient,
                        reinterpret_cast<const char *>(chunk.payload),
                        chunk.length,
                        pdMS_TO_TICKS(UPLINK_SEND_TIMEOUT_MS));
                const uint32_t send_us = static_cast<uint32_t>(esp_timer_get_time() - before_send_us);

                if (ret < 0) {
                    diag_send_fail++;
                    ESP_LOGW(TAG, "[WS_TX_DIAG] send_bin failed ret=%d len=%u us=%lu",
                             ret,
                             static_cast<unsigned>(chunk.length),
                             static_cast<unsigned long>(send_us));
                } else {
                    diag_sent++;
                    diag_bytes_sent += chunk.length;
                    diag_sum_send_us += send_us;
                    if (send_us > diag_max_send_us) {
                        diag_max_send_us = send_us;
                    }
                    if (send_us > UPLINK_SLOW_SEND_US) {
                        diag_send_slow++;
                        ESP_LOGW(TAG, "[WS_TX_DIAG] slow send %lu us, %u bytes",
                                 static_cast<unsigned long>(send_us),
                                 static_cast<unsigned>(chunk.length));
                    }
                }

                freeUplinkChunk(chunk);
            }

            const int64_t now_us = esp_timer_get_time();
            if ((now_us - diag_window_start_us) >= (UPLINK_DIAG_INTERVAL_MS * 1000LL)) {
                const uint32_t enqueued = uplinkDiagEnqueued_.exchange(0, std::memory_order_relaxed);
                const uint32_t dropped_oldest =
                        uplinkDiagDroppedOldest_.exchange(0, std::memory_order_relaxed);

                logUplinkDiagnostics(now_us,
                                   enqueued,
                                   diag_sent,
                                   dropped_oldest,
                                   diag_dropped_disc,
                                   diag_send_fail,
                                   diag_send_slow,
                                   diag_bytes_sent,
                                   diag_sum_send_us,
                                   diag_max_send_us,
                                   queue_depth);
                diag_window_start_us = now_us;
                diag_sent = 0;
                diag_dropped_disc = 0;
                diag_send_fail = 0;
                diag_send_slow = 0;
                diag_bytes_sent = 0;
                diag_sum_send_us = 0;
                diag_max_send_us = 0;
            }
        }

        ESP_LOGI(TAG, "mic_uplink worker stopped");
    }

    esp_err_t WebSocketVoiceClient::sendAudioChunk(const uint8_t *audioData, size_t length) {
        if (!uplinkAccepting_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!audioData || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!isConnected()) {
            return ESP_ERR_INVALID_STATE;
        }

        void *copy = allocateUplinkPayload(length);
        if (!copy) {
            ESP_LOGE(TAG, "Failed to allocate uplink audio buffer (%u bytes)",
                     static_cast<unsigned>(length));
            return ESP_ERR_NO_MEM;
        }

        memcpy(copy, audioData, length);

        UplinkChunk chunk{
                .shutdown = false,
                .payload = static_cast<uint8_t *>(copy),
                .length = static_cast<uint32_t>(length),
        };

        const esp_err_t ret = enqueueUplinkChunk(chunk);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        if (ret == ESP_FAIL) {
            return ESP_FAIL;
        }

        return ret;
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
                if (eventData && eventData->data_ptr && eventData->data_len > 0) {
                    if (eventData->op_code == 0x1) {
                        voiceAssistantManager.onTextData(
                                reinterpret_cast<const uint8_t *>(eventData->data_ptr),
                                static_cast<size_t>(eventData->data_len));
                    } else if (eventData->op_code == 0x2) {
                        voiceAssistantManager.onAudioResponse(
                                reinterpret_cast<const uint8_t *>(eventData->data_ptr),
                                static_cast<size_t>(eventData->data_len));
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
        if (!websocketClient) {
            return ESP_FAIL;
        }

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
        const ConnectionState old_state = state.exchange(newState);
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
        const esp_err_t ret = client->connect();

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
