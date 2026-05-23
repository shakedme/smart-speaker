#include "../include/websocket_voice_client.hpp"
#include "../include/task_affinity.hpp"
#include "../include/uplink_audio_format.hpp"
#include "../include/ima_adpcm.hpp"
#include "../include/voice_assistant_manager.hpp"
#include "../include/voice_config.hpp"
#include "diagnostics_config.hpp"
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
        constexpr uint32_t kUplinkSlowCopyUs = 10000;
        constexpr uint32_t kUplinkSlowEncodeUs = 10000;

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

    void WebSocketVoiceClient::setPlaybackActive(const bool active) {
        const bool was = playbackActive_.exchange(active, std::memory_order_relaxed);
        if (was != active) {
            adpcmState_ = {};
        }
    }

    void WebSocketVoiceClient::setPlaybackStress(const bool stressed) {
        const bool was = playbackStress_.exchange(stressed, std::memory_order_relaxed);
        if (was != stressed && diagnostics_enabled) {
            ESP_LOGI(TAG, "[WS_TX_DIAG] playback stress %s", stressed ? "enabled" : "cleared");
        }
    }

    void WebSocketVoiceClient::setPlaybackMicSuspended(const bool suspended) {
        const bool was = playbackMicSuspended_.exchange(suspended, std::memory_order_relaxed);
        if (was == suspended) {
            return;
        }

        if (diagnostics_enabled) {
            ESP_LOGI(TAG, "[WS_TX_DIAG] mic uplink %s for playback protection",
                     suspended ? "suspended" : "resumed");
        }
        if (suspended) {
            drainUplinkQueue();
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
            drainRefQueue();
#endif
        }
    }

    bool WebSocketVoiceClient::isTransportReady() const {
        return state.load(std::memory_order_acquire) == ConnectionState::CONNECTED &&
               websocketClient != nullptr &&
               esp_websocket_client_is_connected(websocketClient);
    }

    uint32_t WebSocketVoiceClient::computeReconnectBackoffMs() const {
        const uint32_t failures = consecutiveFailures_.load(std::memory_order_relaxed);
        uint32_t delay_ms = static_cast<uint32_t>(config.reconnectBackoffInitialMs);
        for (uint32_t i = 0; i < failures && delay_ms < static_cast<uint32_t>(config.reconnectBackoffMaxMs); ++i) {
            const uint32_t doubled = delay_ms * 2;
            delay_ms = doubled > static_cast<uint32_t>(config.reconnectBackoffMaxMs)
                       ? static_cast<uint32_t>(config.reconnectBackoffMaxMs)
                       : doubled;
        }
        return delay_ms;
    }

    void WebSocketVoiceClient::recordConnectSuccess() {
        consecutiveFailures_.store(0, std::memory_order_relaxed);
    }

    void WebSocketVoiceClient::recordConnectFailure() {
        consecutiveFailures_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t WebSocketVoiceClient::beginConnectAttempt() {
        const uint32_t generation = connectGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
        connectOutcome_.store(CONNECT_OUTCOME_PENDING, std::memory_order_release);
        connectOutcomeGeneration_.store(0, std::memory_order_release);
        return generation;
    }

    void WebSocketVoiceClient::signalConnectOutcome(const uint32_t outcome) {
        const uint32_t generation = connectGeneration_.load(std::memory_order_acquire);
        connectOutcome_.store(outcome, std::memory_order_release);
        connectOutcomeGeneration_.store(generation, std::memory_order_release);
    }

    uint32_t WebSocketVoiceClient::waitForConnectOutcome(const uint32_t generation, const int timeout_ms) {
        const int64_t deadline_us = esp_timer_get_time() + (static_cast<int64_t>(timeout_ms) * 1000LL);
        while (esp_timer_get_time() < deadline_us) {
            if (isTransportReady()) {
                return CONNECT_OUTCOME_SUCCESS;
            }

            const uint32_t outcome_gen = connectOutcomeGeneration_.load(std::memory_order_acquire);
            const uint32_t outcome = connectOutcome_.load(std::memory_order_acquire);
            if (outcome_gen == generation && outcome != CONNECT_OUTCOME_PENDING) {
                return outcome;
            }

            if (connectGeneration_.load(std::memory_order_acquire) != generation) {
                return CONNECT_OUTCOME_FAILURE;
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }

        return CONNECT_OUTCOME_FAILURE;
    }

    void WebSocketVoiceClient::startReconnectLoop() {
        if (reconnectLoopTaskHandle_ != nullptr) {
            return;
        }

        reconnectShutdown_.store(false, std::memory_order_release);
        if (xTaskCreatePinnedToCore(reconnectLoopTask, "ws_reconnect", 4096, this,
                                    kWsReconnectPriority, &reconnectLoopTaskHandle_, kIoCore) != pdPASS) {
            reconnectLoopTaskHandle_ = nullptr;
            ESP_LOGE(TAG, "Failed to start WebSocket reconnect loop task");
        } else {
            ESP_LOGI(TAG, "WebSocket reconnect loop started (backoff %d-%d ms)",
                     config.reconnectBackoffInitialMs, config.reconnectBackoffMaxMs);
        }
    }

    void WebSocketVoiceClient::stopReconnectLoop() {
        reconnectShutdown_.store(true, std::memory_order_release);
        if (reconnectLoopTaskHandle_ != nullptr) {
            vTaskDelete(reconnectLoopTaskHandle_);
            reconnectLoopTaskHandle_ = nullptr;
        }
    }

    void WebSocketVoiceClient::reconnectLoopTask(void *pvParameters) {
        auto *client = static_cast<WebSocketVoiceClient *>(pvParameters);

        while (!client->reconnectShutdown_.load(std::memory_order_acquire)) {
            if (!client->networkAvailable_.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            if (client->isTransportReady()) {
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_LOOP_IDLE_MS));
                continue;
            }

            const uint32_t backoff_ms = client->computeReconnectBackoffMs();
            const uint32_t attempt = client->consecutiveFailures_.load(std::memory_order_relaxed) + 1;
            ESP_LOGI(client->TAG, "WebSocket reconnect in %lu ms (attempt %lu)",
                     static_cast<unsigned long>(backoff_ms),
                     static_cast<unsigned long>(attempt));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));

            if (client->reconnectShutdown_.load(std::memory_order_acquire)) {
                break;
            }
            if (!client->networkAvailable_.load(std::memory_order_acquire)) {
                continue;
            }
            if (client->isTransportReady()) {
                continue;
            }

            if (client->websocketClient) {
                client->destroyWebsocketClient();
            }

            const uint32_t generation = client->beginConnectAttempt();
            const esp_err_t ret = client->connect();
            if (ret != ESP_OK) {
                ESP_LOGE(client->TAG, "WebSocket connect start failed: %s", esp_err_to_name(ret));
                client->setConnectionState(ConnectionState::DISCONNECTED);
                client->recordConnectFailure();
                continue;
            }

            const uint32_t outcome = client->waitForConnectOutcome(generation, RECONNECT_CONNECT_WAIT_MS);
            if (outcome == CONNECT_OUTCOME_SUCCESS) {
                ESP_LOGI(client->TAG, "WebSocket connected to %s", client->config.serverUrl.c_str());
                client->recordConnectSuccess();
                continue;
            }

            ESP_LOGW(client->TAG, "WebSocket connect attempt failed (outcome=%lu)",
                     static_cast<unsigned long>(outcome));
            if (client->websocketClient) {
                client->destroyWebsocketClient();
            }
            client->setConnectionState(ConnectionState::DISCONNECTED);
            client->recordConnectFailure();
        }

        client->reconnectLoopTaskHandle_ = nullptr;
        vTaskDelete(nullptr);
    }

    void WebSocketVoiceClient::markTransportDisconnected(const char *reason) {
        const ConnectionState current = state.load(std::memory_order_relaxed);
        if (current == ConnectionState::CONNECTED || current == ConnectionState::CONNECTING) {
            ESP_LOGW(TAG, "WebSocket transport down (%s), marking disconnected",
                     reason ? reason : "?");
            setConnectionState(ConnectionState::DISCONNECTED);
            isClientRunning = false;
            drainUplinkQueue();
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
            drainRefQueue();
#endif
        }
    }

    void WebSocketVoiceClient::onNetworkLost() {
        ESP_LOGW(TAG, "Network lost, stopping WebSocket client");
        networkAvailable_.store(false, std::memory_order_release);
        if (state.load(std::memory_order_relaxed) == ConnectionState::CONNECTED ||
            state.load(std::memory_order_relaxed) == ConnectionState::CONNECTING) {
            setConnectionState(ConnectionState::DISCONNECTED);
        }
        if (websocketClient && isClientRunning) {
            esp_websocket_client_stop(websocketClient);
            isClientRunning = false;
        }
        drainUplinkQueue();
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
        drainRefQueue();
#endif
    }

    void WebSocketVoiceClient::onNetworkAvailable() {
        ESP_LOGI(TAG, "Network available, resuming WebSocket reconnect loop");
        networkAvailable_.store(true, std::memory_order_release);
        consecutiveFailures_.store(0, std::memory_order_relaxed);
        if (websocketClient) {
            destroyWebsocketClient();
        }
        setConnectionState(ConnectionState::DISCONNECTED);
    }

    WebSocketVoiceClient::WebSocketVoiceClient(VoiceAssistantManager &voiceAssistantManager)
            : config(),
              state(ConnectionState::DISCONNECTED),
              websocketClient(nullptr),
              stateMutex(nullptr),
              reconnectLoopTaskHandle_(nullptr),
              isDestroying(false),
              voiceAssistantManager(voiceAssistantManager),
              initialized(false),
              isClientRunning(false) {

        config.serverUrl = CONFIG_WEBSOCKET_SERVER_URL;
        config.keepaliveIntervalS = CONFIG_WS_KEEPALIVE_INTERVAL_S;
        config.pingIntervalS = CONFIG_WS_PING_INTERVAL_S;
        config.chunkSize = CONFIG_WS_CHUNK_SIZE;
        config.reconnectBackoffInitialMs = CONFIG_WS_RECONNECT_BACKOFF_INITIAL_MS;
        config.reconnectBackoffMaxMs = CONFIG_WS_RECONNECT_BACKOFF_MAX_MS;

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

        initialized = true;
        startReconnectLoop();
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

        uplinkFreeQueue_ = xQueueCreate(UPLINK_QUEUE_DEPTH, sizeof(uint8_t *));
        if (!uplinkFreeQueue_) {
            vQueueDelete(uplinkQueue_);
            uplinkQueue_ = nullptr;
            ESP_LOGE(TAG, "Failed to create uplink buffer pool queue");
            return ESP_FAIL;
        }

        for (size_t i = 0; i < UPLINK_QUEUE_DEPTH; ++i) {
            auto *buffer = static_cast<uint8_t *>(allocateUplinkPayload(UPLINK_MAX_CHUNK_BYTES));
            if (!buffer || xQueueSend(uplinkFreeQueue_, &buffer, 0) != pdTRUE) {
                if (buffer) {
                    heap_caps_free(buffer);
                }
                shutdownUplinkWorker();
                ESP_LOGE(TAG, "Failed to allocate uplink buffer pool");
                return ESP_ERR_NO_MEM;
            }
        }

        uplinkStoppedSem_ = xSemaphoreCreateBinary();
        if (!uplinkStoppedSem_) {
            shutdownUplinkWorker();
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
                kMicUplinkPriority,
                uplinkTaskStack_,
                uplinkTaskBuffer_,
                kMicUplinkCore);

        if (!uplinkTaskHandle_) {
            shutdownUplinkWorker();
            ESP_LOGE(TAG, "Failed to create mic_uplink task");
            return ESP_FAIL;
        }

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
        refUplinkQueue_ = xQueueCreate(REF_UPLINK_QUEUE_DEPTH, sizeof(UplinkChunk));
        refUplinkFreeQueue_ = xQueueCreate(REF_UPLINK_QUEUE_DEPTH, sizeof(uint8_t *));
        if (!refUplinkQueue_ || !refUplinkFreeQueue_) {
            shutdownUplinkWorker();
            ESP_LOGE(TAG, "Failed to create playback-ref uplink queues");
            return ESP_FAIL;
        }

        for (size_t i = 0; i < REF_UPLINK_QUEUE_DEPTH; ++i) {
            auto *buffer = static_cast<uint8_t *>(
                    allocateUplinkPayload(1 + kUplinkFramedHeaderSize + REF_UPLINK_MAX_FRAME_BYTES));
            if (!buffer || xQueueSend(refUplinkFreeQueue_, &buffer, 0) != pdTRUE) {
                if (buffer) {
                    heap_caps_free(buffer);
                }
                shutdownUplinkWorker();
                ESP_LOGE(TAG, "Failed to allocate playback-ref buffer pool");
                return ESP_ERR_NO_MEM;
            }
        }
#endif

        ESP_LOGI(TAG, "mic_uplink worker started (stack %u words, queue depth %u, chunk %u bytes)",
                 static_cast<unsigned>(UPLINK_TASK_STACK_WORDS),
                 static_cast<unsigned>(UPLINK_QUEUE_DEPTH),
                 static_cast<unsigned>(UPLINK_MAX_CHUNK_BYTES));
        return ESP_OK;
    }

    void WebSocketVoiceClient::freeUplinkChunk(UplinkChunk &chunk) {
        if (chunk.payload) {
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
            if (chunk.is_framed &&
                chunk.frame_type == static_cast<uint8_t>(AudioFrameType::PlaybackRef)) {
                if (refUplinkFreeQueue_ &&
                    xQueueSend(refUplinkFreeQueue_, &chunk.payload, 0) != pdTRUE) {
                    heap_caps_free(chunk.payload);
                }
            } else {
                releaseUplinkBuffer(chunk.payload);
            }
#else
            releaseUplinkBuffer(chunk.payload);
#endif
            chunk.payload = nullptr;
        }
        chunk.length = 0;
        chunk.shutdown = false;
        chunk.is_framed = false;
        chunk.frame_type = 0;
    }

    uint8_t *WebSocketVoiceClient::acquireUplinkBuffer() {
        if (!uplinkFreeQueue_) {
            return nullptr;
        }

        uint8_t *payload = nullptr;
        if (xQueueReceive(uplinkFreeQueue_, &payload, 0) == pdTRUE) {
            return payload;
        }

        if (dropOldestUplinkChunk() &&
            xQueueReceive(uplinkFreeQueue_, &payload, 0) == pdTRUE) {
            if (diagnostics_enabled) {
                uplinkDiagDroppedOldest_.fetch_add(1, std::memory_order_relaxed);
            }
            return payload;
        }

        return nullptr;
    }

    void WebSocketVoiceClient::releaseUplinkBuffer(uint8_t *payload) {
        if (!payload) {
            return;
        }
        if (!uplinkFreeQueue_ || xQueueSend(uplinkFreeQueue_, &payload, 0) != pdTRUE) {
            heap_caps_free(payload);
        }
    }

    void WebSocketVoiceClient::destroyUplinkBufferPool() {
        if (!uplinkFreeQueue_) {
            return;
        }

        uint8_t *payload = nullptr;
        while (xQueueReceive(uplinkFreeQueue_, &payload, 0) == pdTRUE) {
            heap_caps_free(payload);
        }
        vQueueDelete(uplinkFreeQueue_);
        uplinkFreeQueue_ = nullptr;
    }

    void WebSocketVoiceClient::drainUplinkQueue() {
        if (!uplinkQueue_) {
            return;
        }

        UplinkChunk pending{};
        while (xQueueReceive(uplinkQueue_, &pending, 0) == pdTRUE) {
            if (pending.shutdown) {
                xQueueSendToFront(uplinkQueue_, &pending, 0);
                break;
            }
            freeUplinkChunk(pending);
        }
    }

    void WebSocketVoiceClient::shutdownUplinkWorker() {
        uplinkAccepting_.store(false);

        if (uplinkQueue_) {
            UplinkChunk shutdown_chunk{
                    .shutdown = true,
                    .is_framed = false,
                    .frame_type = 0,
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
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
        drainRefQueue();
        if (refUplinkQueue_) {
            vQueueDelete(refUplinkQueue_);
            refUplinkQueue_ = nullptr;
        }
        if (refUplinkFreeQueue_) {
            uint8_t *payload = nullptr;
            while (xQueueReceive(refUplinkFreeQueue_, &payload, 0) == pdTRUE) {
                heap_caps_free(payload);
            }
            vQueueDelete(refUplinkFreeQueue_);
            refUplinkFreeQueue_ = nullptr;
        }
#endif

        if (uplinkQueue_) {
            vQueueDelete(uplinkQueue_);
            uplinkQueue_ = nullptr;
        }
        destroyUplinkBufferPool();

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
            if (diagnostics_enabled) {
                uplinkDiagEnqueued_.fetch_add(1, std::memory_order_relaxed);
            }
            return ESP_OK;
        }

        if (dropOldestUplinkChunk()) {
            if (diagnostics_enabled) {
                uplinkDiagDroppedOldest_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            freeUplinkChunk(chunk);
            return ESP_FAIL;
        }

        if (xQueueSend(uplinkQueue_, &chunk, 0) == pdTRUE) {
            if (diagnostics_enabled) {
                uplinkDiagEnqueued_.fetch_add(1, std::memory_order_relaxed);
            }
            return ESP_OK;
        }

        freeUplinkChunk(chunk);
        return ESP_FAIL;
    }

    bool WebSocketVoiceClient::shouldDropForPlaybackCongestion(const uint32_t queue_depth) const {
        if (!playbackActive_.load(std::memory_order_relaxed)) {
            return false;
        }

        if (queue_depth >= UPLINK_PLAYBACK_QUEUE_MAX) {
            return true;
        }

        return false;
    }

    esp_err_t WebSocketVoiceClient::buildUplinkPayload(const uint8_t *audioData,
                                                       const size_t length,
                                                       uint8_t **out_payload,
                                                       uint32_t *out_length) {
        if (!audioData || !out_payload || !out_length || length == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        const bool use_adpcm = length % sizeof(int16_t) == 0;

        if (use_adpcm) {
            const size_t sample_count = length / sizeof(int16_t);
            const size_t adpcm_bytes = (sample_count + 1) / 2;
            const size_t total = kUplinkFrameHeaderSize + adpcm_bytes;
            uint8_t *buf = static_cast<uint8_t *>(allocateUplinkPayload(total));
            if (!buf) {
                return ESP_ERR_NO_MEM;
            }

            buf[0] = kUplinkFormatImaAdpcm;
            const size_t encoded = imaAdpcmEncode(
                    reinterpret_cast<const int16_t *>(audioData),
                    sample_count,
                    buf + kUplinkFrameHeaderSize,
                    adpcm_bytes,
                    adpcmState_);
            if (encoded == 0) {
                heap_caps_free(buf);
                return ESP_FAIL;
            }

            *out_payload = buf;
            *out_length = static_cast<uint32_t>(kUplinkFrameHeaderSize + encoded);
            return ESP_OK;
        }

        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM

    uint32_t WebSocketVoiceClient::playbackRefQueueDepth() const {
        return refUplinkQueue_
               ? static_cast<uint32_t>(uxQueueMessagesWaiting(refUplinkQueue_))
               : 0;
    }

    void WebSocketVoiceClient::drainRefQueue() {
        if (!refUplinkQueue_) {
            return;
        }

        UplinkChunk pending{};
        while (xQueueReceive(refUplinkQueue_, &pending, 0) == pdTRUE) {
            freeUplinkChunk(pending);
        }
    }

    bool WebSocketVoiceClient::dropOldestRefChunk() {
        UplinkChunk oldest{};
        if (xQueueReceive(refUplinkQueue_, &oldest, 0) != pdTRUE) {
            return false;
        }
        freeUplinkChunk(oldest);
        return true;
    }

    esp_err_t WebSocketVoiceClient::buildFramedPayload(const AudioFrameType frame_type,
                                                       const int16_t *pcm,
                                                       const uint32_t num_samples,
                                                       const uint32_t sample_rate_hz,
                                                       const uint16_t channel_count,
                                                       const uint32_t sequence,
                                                       const int64_t timestamp_us,
                                                       uint8_t *out_buf,
                                                       const size_t out_capacity,
                                                       uint32_t *out_length) {
        if (!pcm || !out_buf || !out_length || num_samples == 0 || channel_count == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        const size_t sample_count = static_cast<size_t>(num_samples) * channel_count;
        const bool use_adpcm = sample_count > 1;
        const size_t payload_bytes = use_adpcm
                                     ? FRAMED_ADPCM_BLOCK_HEADER_BYTES + ((sample_count - 1) + 1) / 2
                                     : sample_count * sizeof(int16_t);
        const size_t total = 1 + kUplinkFramedHeaderSize + payload_bytes;
        if (total > out_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }

        out_buf[0] = kUplinkFormatFramed;
        auto *header = reinterpret_cast<UplinkAudioFrameHeader *>(out_buf + 1);
        header->frame_type = static_cast<uint8_t>(frame_type);
        header->sample_format = static_cast<uint8_t>(
                use_adpcm ? UplinkSampleFormat::ImaAdpcmBlock : UplinkSampleFormat::Int16Pcm);
        header->channel_count = channel_count;
        header->sample_rate = sample_rate_hz;
        header->sequence = sequence;
        header->timestamp_us = timestamp_us;
        header->num_samples = num_samples;

        uint8_t *payload = out_buf + 1 + kUplinkFramedHeaderSize;
        if (use_adpcm) {
            const int16_t predictor = pcm[0];
            payload[0] = static_cast<uint8_t>(predictor & 0xff);
            payload[1] = static_cast<uint8_t>((static_cast<uint16_t>(predictor) >> 8) & 0xff);
            payload[2] = 0;  // IMA step-table index.
            payload[3] = 0;  // Reserved for alignment/future flags.

            ImaAdpcmState block_state{};
            block_state.predictor = predictor;
            block_state.index = 0;
            const size_t encoded = imaAdpcmEncode(
                    pcm + 1,
                    sample_count - 1,
                    payload + FRAMED_ADPCM_BLOCK_HEADER_BYTES,
                    payload_bytes - FRAMED_ADPCM_BLOCK_HEADER_BYTES,
                    block_state);
            if (encoded == 0 && sample_count > 1) {
                return ESP_FAIL;
            }
        } else {
            memcpy(payload, pcm, payload_bytes);
        }

        *out_length = static_cast<uint32_t>(total);
        return ESP_OK;
    }

    esp_err_t WebSocketVoiceClient::enqueueRefChunk(UplinkChunk chunk) {
        if (!refUplinkQueue_ || !uplinkAccepting_.load()) {
            freeUplinkChunk(chunk);
            return ESP_ERR_INVALID_STATE;
        }

        const uint32_t mic_depth = uplinkQueue_
                                   ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplinkQueue_))
                                   : 0;
        if (mic_depth >= UPLINK_MIC_QUEUE_REF_PAUSE_DEPTH ||
            playbackMicSuspended_.load(std::memory_order_relaxed)) {
            freeUplinkChunk(chunk);
            uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
            return ESP_ERR_TIMEOUT;
        }

        if (xQueueSend(refUplinkQueue_, &chunk, 0) == pdTRUE) {
            return ESP_OK;
        }

        if (dropOldestRefChunk()) {
            uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            freeUplinkChunk(chunk);
            uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
            return ESP_FAIL;
        }

        if (xQueueSend(refUplinkQueue_, &chunk, 0) == pdTRUE) {
            return ESP_OK;
        }

        freeUplinkChunk(chunk);
        uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
        return ESP_FAIL;
    }

    esp_err_t WebSocketVoiceClient::sendFramedAudio(const AudioFrameType frame_type,
                                                    const int16_t *pcm,
                                                    const uint32_t num_samples,
                                                    const uint32_t sample_rate_hz,
                                                    const uint16_t channel_count,
                                                    const uint32_t sequence,
                                                    const int64_t timestamp_us) {
        if (!uplinkAccepting_.load() || !pcm || num_samples == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!isTransportReady()) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t *payload = nullptr;
        if (frame_type == AudioFrameType::PlaybackRef) {
            if (!refUplinkFreeQueue_ ||
                xQueueReceive(refUplinkFreeQueue_, &payload, 0) != pdTRUE) {
                if (dropOldestRefChunk() &&
                    xQueueReceive(refUplinkFreeQueue_, &payload, 0) == pdTRUE) {
                    uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    uplinkDiagRefDropped_.fetch_add(1, std::memory_order_relaxed);
                    return ESP_ERR_TIMEOUT;
                }
            }
        } else {
            payload = acquireUplinkBuffer();
            if (!payload) {
                uplinkDiagMicDropped_.fetch_add(1, std::memory_order_relaxed);
                return ESP_OK;
            }
        }

        uint32_t length = 0;
        const size_t payload_capacity = frame_type == AudioFrameType::PlaybackRef
                                        ? (1 + kUplinkFramedHeaderSize + REF_UPLINK_MAX_FRAME_BYTES)
                                        : UPLINK_MAX_CHUNK_BYTES;
        const esp_err_t build_ret = buildFramedPayload(
                frame_type, pcm, num_samples, sample_rate_hz, channel_count,
                sequence, timestamp_us, payload, payload_capacity, &length);
        if (build_ret != ESP_OK) {
            if (frame_type == AudioFrameType::PlaybackRef) {
                if (refUplinkFreeQueue_) {
                    xQueueSend(refUplinkFreeQueue_, &payload, 0);
                } else {
                    heap_caps_free(payload);
                }
            } else {
                releaseUplinkBuffer(payload);
            }
            return build_ret;
        }

        UplinkChunk chunk{
                .shutdown = false,
                .is_framed = true,
                .frame_type = static_cast<uint8_t>(frame_type),
                .payload = payload,
                .length = length,
        };

        if (frame_type == AudioFrameType::PlaybackRef) {
            return enqueueRefChunk(chunk);
        }

        const esp_err_t ret = enqueueUplinkChunk(chunk);
        if (ret != ESP_OK) {
            uplinkDiagMicDropped_.fetch_add(1, std::memory_order_relaxed);
        }
        return ret;
    }

    bool WebSocketVoiceClient::sendPreparedChunk(const UplinkChunk &chunk,
                                                 const bool playback,
                                                 const bool stress,
                                                 uint32_t &diag_sent,
                                                 uint32_t &diag_send_fail,
                                                 uint32_t &diag_send_slow,
                                                 uint64_t &diag_bytes_sent,
                                                 uint64_t &diag_sum_send_us,
                                                 uint32_t &diag_max_send_us) {
        const int send_timeout_ms = playback ? UPLINK_PLAYBACK_SEND_TIMEOUT_MS
                                             : UPLINK_SEND_TIMEOUT_MS;
        const uint8_t *send_payload = chunk.payload;
        const uint32_t send_length = chunk.length;

        if (!isTransportReady()) {
            if (diagnostics_enabled) {
                diag_send_fail++;
            }
            markTransportDisconnected("uplink_send_failed");
            return false;
        }

        const int64_t before_send_us = esp_timer_get_time();
        const int ret = esp_websocket_client_send_bin(
                websocketClient,
                reinterpret_cast<const char *>(send_payload),
                send_length,
                pdMS_TO_TICKS(send_timeout_ms));
        const uint32_t send_us = static_cast<uint32_t>(esp_timer_get_time() - before_send_us);

        if (ret < 0) {
            if (diagnostics_enabled) {
                diag_send_fail++;
            }
            markTransportDisconnected("uplink_send_failed");
            return false;
        }
        if (ret != static_cast<int>(send_length)) {
            // ret==0 usually means TCP send buffer full under WiFi/Spotify load. Drop frame,
            // keep the session alive instead of tearing down the whole WebSocket.
            if (diagnostics_enabled) {
                diag_send_fail++;
            }
            return false;
        }

        if (diagnostics_enabled) {
            diag_sent++;
            diag_bytes_sent += static_cast<uint32_t>(ret);
            diag_sum_send_us += send_us;
            if (send_us > diag_max_send_us) {
                diag_max_send_us = send_us;
            }
            const uint32_t slow_threshold = playback ? UPLINK_SLOW_SEND_US / 2
                                                       : UPLINK_SLOW_SEND_US;
            if (send_us > slow_threshold) {
                diag_send_slow++;
            }
        }
        (void) stress;
        return true;
    }

#endif

    void WebSocketVoiceClient::logUplinkDiagnostics(const int64_t now_us,
                                                    const uint32_t enqueued,
                                                    const uint32_t sent,
                                                    const uint32_t dropped_oldest,
                                                    const uint32_t dropped_playback,
                                                    const uint32_t dropped_disc,
                                                    const uint32_t send_fail,
                                                    const uint32_t send_slow,
                                                    const uint64_t bytes_sent,
                                                    const uint64_t sum_send_us,
                                                    const uint32_t max_send_us,
                                                    const uint32_t queue_depth) {
        if (!diagnostics_enabled) {
            return;
        }

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
                 "[WS_TX_DIAG] enqueued=%lu sent=%lu drop_old=%lu drop_pb=%lu drop_disc=%lu send_fail=%lu slow=%lu "
                 "bytes=%lu ~%luHz send_us(avg=%lu max=%lu) q=%lu stress=%d mic_suspend=%d task=%s dram=%lu"
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
                 " mic(s=%lu d=%lu) ref(s=%lu d=%lu qref=%lu)"
#endif
                 ,
                 static_cast<unsigned long>(enqueued),
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(dropped_oldest),
                 static_cast<unsigned long>(dropped_playback),
                 static_cast<unsigned long>(dropped_disc),
                 static_cast<unsigned long>(send_fail),
                 static_cast<unsigned long>(send_slow),
                 static_cast<unsigned long>(bytes_sent),
                 static_cast<unsigned long>(effective_hz),
                 static_cast<unsigned long>(avg_send_us),
                 static_cast<unsigned long>(max_send_us),
                 static_cast<unsigned long>(queue_depth),
                 playbackStress_.load(std::memory_order_relaxed) ? 1 : 0,
                 playbackMicSuspended_.load(std::memory_order_relaxed) ? 1 : 0,
                 task_name,
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL))
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
                 ,
                 static_cast<unsigned long>(uplinkDiagMicSent_.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long>(uplinkDiagMicDropped_.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long>(uplinkDiagRefSent_.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long>(uplinkDiagRefDropped_.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long>(playbackRefQueueDepth())
#endif
        );
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
            bool shutdown_after_send = false;

            if (chunk.shutdown) {
                break;
            }

            const uint32_t queue_depth = uplinkQueue_
                                         ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplinkQueue_))
                                         : 0;

            if (!isTransportReady()) {
                markTransportDisconnected("uplink_worker");
                if (diagnostics_enabled) {
                    diag_dropped_disc++;
                }
                freeUplinkChunk(chunk);
            } else if (playbackMicSuspended_.load(std::memory_order_relaxed)) {
                freeUplinkChunk(chunk);
            } else if (!chunk.is_framed && shouldDropForPlaybackCongestion(queue_depth)) {
                if (diagnostics_enabled) {
                    uplinkDiagDroppedPlayback_.fetch_add(1, std::memory_order_relaxed);
                }
                freeUplinkChunk(chunk);
            } else {
                const bool playback = playbackActive_.load(std::memory_order_relaxed);
                const bool stress = playbackStress_.load(std::memory_order_relaxed);
                const int send_timeout_ms = playback ? UPLINK_PLAYBACK_SEND_TIMEOUT_MS
                                                     : UPLINK_SEND_TIMEOUT_MS;
                const uint8_t *send_payload = chunk.payload;
                uint32_t send_length = chunk.length;
                uint8_t *encoded_payload = nullptr;
                uint32_t encoded_length = 0;

                if (playback && !chunk.is_framed) {
                    const uint32_t aggregate_target = stress
                                                      ? UPLINK_STRESS_AGGREGATE_BYTES
                                                      : UPLINK_PLAYBACK_AGGREGATE_BYTES;
                    while (chunk.length < aggregate_target) {
                        UplinkChunk next{};
                        if (xQueueReceive(uplinkQueue_, &next,
                                          pdMS_TO_TICKS(UPLINK_PLAYBACK_AGGREGATE_WAIT_MS)) != pdTRUE) {
                            break;
                        }
                        if (next.shutdown) {
                            shutdown_after_send = true;
                            break;
                        }
                        if (next.is_framed || !next.payload ||
                            chunk.length + next.length > UPLINK_MAX_CHUNK_BYTES ||
                            chunk.length + next.length > aggregate_target) {
                            freeUplinkChunk(next);
                            break;
                        }
                        memcpy(chunk.payload + chunk.length, next.payload, next.length);
                        chunk.length += next.length;
                        freeUplinkChunk(next);
                    }

                    const int64_t before_encode_us = esp_timer_get_time();
                    const esp_err_t encode_ret = buildUplinkPayload(
                            chunk.payload,
                            chunk.length,
                            &encoded_payload,
                            &encoded_length);
                    const uint32_t encode_us =
                            static_cast<uint32_t>(esp_timer_get_time() - before_encode_us);
                    if (encode_ret == ESP_OK && encoded_payload && encoded_length > 0) {
                        send_payload = encoded_payload;
                        send_length = encoded_length;
                    } else {
                        if (diagnostics_enabled) {
                            diag_send_fail++;
                            ESP_LOGW(TAG, "[WS_TX_DIAG] encode failed: %s (%u bytes, %lu us)",
                                     esp_err_to_name(encode_ret),
                                     static_cast<unsigned>(chunk.length),
                                     static_cast<unsigned long>(encode_us));
                        }
                        freeUplinkChunk(chunk);
                        continue;
                    }
                    if (diagnostics_enabled && encode_us > kUplinkSlowEncodeUs) {
                        ESP_LOGW(TAG, "[WS_TX_DIAG] slow encode %lu us, %u -> %u bytes",
                                 static_cast<unsigned long>(encode_us),
                                 static_cast<unsigned>(chunk.length),
                                 static_cast<unsigned>(encoded_length));
                    }
                }

                if (!isTransportReady()) {
                    if (diagnostics_enabled) {
                        diag_dropped_disc++;
                    }
                    if (encoded_payload) {
                        heap_caps_free(encoded_payload);
                    }
                    freeUplinkChunk(chunk);
                    continue;
                }

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
                (void) send_payload;
                (void) send_length;
                (void) send_timeout_ms;
                const bool sent_ok = sendPreparedChunk(
                        chunk, playback, stress, diag_sent, diag_send_fail, diag_send_slow,
                        diag_bytes_sent, diag_sum_send_us, diag_max_send_us);
                if (sent_ok && chunk.is_framed &&
                    chunk.frame_type == static_cast<uint8_t>(AudioFrameType::Mic)) {
                    uplinkDiagMicSent_.fetch_add(1, std::memory_order_relaxed);
                }
#else
                const int64_t before_send_us = esp_timer_get_time();
                const int ret = esp_websocket_client_send_bin(
                        websocketClient,
                        reinterpret_cast<const char *>(send_payload),
                        send_length,
                        pdMS_TO_TICKS(send_timeout_ms));
                const uint32_t send_us = static_cast<uint32_t>(esp_timer_get_time() - before_send_us);

                if (ret < 0) {
                    if (diagnostics_enabled) {
                        diag_send_fail++;
                        ESP_LOGW(TAG, "[WS_TX_DIAG] send_bin failed ret=%d len=%u us=%lu",
                                 ret,
                                 static_cast<unsigned>(send_length),
                                 static_cast<unsigned long>(send_us));
                    }
                    markTransportDisconnected("uplink_send_failed");
                } else if (ret != static_cast<int>(send_length)) {
                    if (diagnostics_enabled) {
                        diag_send_fail++;
                        ESP_LOGW(TAG, "[WS_TX_DIAG] short send ret=%d len=%u us=%lu",
                                 ret,
                                 static_cast<unsigned>(send_length),
                                 static_cast<unsigned long>(send_us));
                    }
                    markTransportDisconnected("uplink_short_send");
                } else {
                    if (diagnostics_enabled) {
                        diag_sent++;
                        diag_bytes_sent += static_cast<uint32_t>(ret);
                        diag_sum_send_us += send_us;
                        if (send_us > diag_max_send_us) {
                            diag_max_send_us = send_us;
                        }
                        const uint32_t slow_threshold = playback ? UPLINK_SLOW_SEND_US / 2
                                                                   : UPLINK_SLOW_SEND_US;
                        if (send_us > slow_threshold) {
                            diag_send_slow++;
                            ESP_LOGW(TAG, "[WS_TX_DIAG] slow send %lu us, %u bytes",
                                     static_cast<unsigned long>(send_us),
                                     static_cast<unsigned>(send_length));
                        }
                    }
                }
#endif

                if (encoded_payload) {
                    heap_caps_free(encoded_payload);
                }
                freeUplinkChunk(chunk);

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
                if (sent_ok && refUplinkQueue_) {
                    uint32_t ref_sent_this_cycle = 0;
                    while (ref_sent_this_cycle < UPLINK_REF_FRAMES_PER_MIC_SEND &&
                           uxQueueMessagesWaiting(uplinkQueue_) < UPLINK_MIC_QUEUE_REF_PAUSE_DEPTH) {
                        UplinkChunk ref_chunk{};
                        if (xQueueReceive(refUplinkQueue_, &ref_chunk, 0) != pdTRUE) {
                            break;
                        }
                        if (sendPreparedChunk(ref_chunk, playback, stress, diag_sent, diag_send_fail,
                                              diag_send_slow, diag_bytes_sent, diag_sum_send_us,
                                              diag_max_send_us)) {
                            uplinkDiagRefSent_.fetch_add(1, std::memory_order_relaxed);
                            ref_sent_this_cycle++;
                        } else {
                            freeUplinkChunk(ref_chunk);
                            break;
                        }
                        freeUplinkChunk(ref_chunk);
                    }
                }
#endif
            }

            if (diagnostics_enabled) {
                const int64_t now_us = esp_timer_get_time();
                if ((now_us - diag_window_start_us) >= (UPLINK_DIAG_INTERVAL_MS * 1000LL)) {
                    const uint32_t enqueued = uplinkDiagEnqueued_.exchange(0, std::memory_order_relaxed);
                    const uint32_t dropped_oldest =
                            uplinkDiagDroppedOldest_.exchange(0, std::memory_order_relaxed);
                    const uint32_t dropped_playback =
                            uplinkDiagDroppedPlayback_.exchange(0, std::memory_order_relaxed);

                    logUplinkDiagnostics(now_us,
                                       enqueued,
                                       diag_sent,
                                       dropped_oldest,
                                       dropped_playback,
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
            if (shutdown_after_send) {
                break;
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
        if (playbackMicSuspended_.load(std::memory_order_relaxed)) {
            return ESP_OK;
        }
        if (!isTransportReady()) {
            return ESP_ERR_INVALID_STATE;
        }

        const uint32_t queue_depth = uplinkQueue_
                                     ? static_cast<uint32_t>(uxQueueMessagesWaiting(uplinkQueue_))
                                     : 0;
        if (shouldDropForPlaybackCongestion(queue_depth)) {
            if (diagnostics_enabled) {
                uplinkDiagDroppedPlayback_.fetch_add(1, std::memory_order_relaxed);
            }
            return ESP_OK;
        }
        if (length > UPLINK_MAX_CHUNK_BYTES) {
            if (diagnostics_enabled) {
                ESP_LOGW(TAG, "[WS_TX_DIAG] dropping oversized uplink chunk %u > %u bytes",
                         static_cast<unsigned>(length),
                         static_cast<unsigned>(UPLINK_MAX_CHUNK_BYTES));
            }
            return ESP_ERR_INVALID_SIZE;
        }

        const int64_t before_copy_us = esp_timer_get_time();
        uint8_t *payload = acquireUplinkBuffer();
        if (!payload) {
            if (diagnostics_enabled) {
                uplinkDiagDroppedOldest_.fetch_add(1, std::memory_order_relaxed);
            }
            return ESP_OK;
        }
        memcpy(payload, audioData, length);
        const uint32_t copy_us = static_cast<uint32_t>(esp_timer_get_time() - before_copy_us);
        if (diagnostics_enabled && copy_us > kUplinkSlowCopyUs) {
            ESP_LOGW(TAG, "[WS_TX_DIAG] slow capture copy %lu us, %u bytes",
                     static_cast<unsigned long>(copy_us),
                     static_cast<unsigned>(length));
        }

        UplinkChunk chunk{
                .shutdown = false,
                .is_framed = false,
                .frame_type = 0,
                .payload = payload,
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
        ESP_LOGI(TAG, "WebSocket client started (awaiting WEBSOCKET_EVENT_CONNECTED)");
        return ESP_OK;
    }

    void WebSocketVoiceClient::disconnect() {
        stopReconnectLoop();

        if (websocketClient) {
            destroyWebsocketClient();
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

    void WebSocketVoiceClient::handleDownlinkFragment(esp_websocket_event_data_t *eventData,
                                                      const bool is_binary) {
        if (!eventData || !eventData->data_ptr || eventData->data_len <= 0) {
            return;
        }

        const int payload_len = eventData->payload_len;
        if (payload_len <= 0) {
            return;
        }
        if (static_cast<size_t>(payload_len) > DOWNLINK_MAX_PAYLOAD_BYTES) {
            ESP_LOGW(TAG, "Ignoring oversized downlink payload (%d bytes)", payload_len);
            if (is_binary) {
                downlinkBinaryBuffer_.clear();
            } else {
                downlinkTextBuffer_.clear();
            }
            return;
        }

        auto &buffer = is_binary ? downlinkBinaryBuffer_ : downlinkTextBuffer_;
        const size_t chunk_offset = static_cast<size_t>(eventData->payload_offset);
        const size_t chunk_len = static_cast<size_t>(eventData->data_len);
        const size_t chunk_end = chunk_offset + chunk_len;
        const size_t expected_len = static_cast<size_t>(payload_len);

        if (chunk_offset == 0) {
            buffer.clear();
            buffer.reserve(expected_len);
        } else if (buffer.empty()) {
            ESP_LOGW(TAG, "Downlink fragment without start (offset=%u)", static_cast<unsigned>(chunk_offset));
            return;
        }

        if (chunk_end > expected_len) {
            ESP_LOGW(TAG, "Downlink fragment exceeds payload length (%u > %u)",
                     static_cast<unsigned>(chunk_end),
                     static_cast<unsigned>(expected_len));
            buffer.clear();
            return;
        }

        if (buffer.size() < chunk_end) {
            buffer.resize(chunk_end);
        }
        memcpy(buffer.data() + chunk_offset, eventData->data_ptr, chunk_len);

        if (chunk_end < expected_len) {
            return;
        }

        if (is_binary) {
            voiceAssistantManager.onAudioResponse(buffer.data(), buffer.size());
        } else {
            voiceAssistantManager.onTextData(buffer.data(), buffer.size());
        }
        buffer.clear();
    }

    void WebSocketVoiceClient::handleWebsocketEvent(esp_websocket_event_id_t eventId,
                                                    esp_websocket_event_data_t *eventData) {
        switch (eventId) {
            case WEBSOCKET_EVENT_CONNECTED:
                recordConnectSuccess();
                signalConnectOutcome(CONNECT_OUTCOME_SUCCESS);
                setConnectionState(ConnectionState::CONNECTED);
                break;

            case WEBSOCKET_EVENT_DISCONNECTED: {
                downlinkBinaryBuffer_.clear();
                downlinkTextBuffer_.clear();
                isClientRunning = false;
                const ConnectionState previous = state.load(std::memory_order_relaxed);
                if (previous == ConnectionState::CONNECTING) {
                    signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
                }
                setConnectionState(ConnectionState::DISCONNECTED);
                drainUplinkQueue();
                break;
            }

            case WEBSOCKET_EVENT_DATA:
                if (eventData && eventData->data_ptr && eventData->data_len > 0) {
                    if (eventData->op_code == 0x1) {
                        handleDownlinkFragment(eventData, false);
                    } else if (eventData->op_code == 0x2) {
                        handleDownlinkFragment(eventData, true);
                    }
                }
                break;

            case WEBSOCKET_EVENT_ERROR: {
                downlinkBinaryBuffer_.clear();
                downlinkTextBuffer_.clear();
                isClientRunning = false;
                const ConnectionState previous = state.load(std::memory_order_relaxed);
                if (previous == ConnectionState::CONNECTING) {
                    signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
                }
                setConnectionState(ConnectionState::DISCONNECTED);
                drainUplinkQueue();
                break;
            }

            default:
                break;
        }
    }

    esp_err_t WebSocketVoiceClient::createWebsocketClient() {
        esp_websocket_client_config_t websocket_cfg = {};
        websocket_cfg.uri = config.serverUrl.c_str();
        websocket_cfg.keep_alive_idle = config.keepaliveIntervalS;
        websocket_cfg.ping_interval_sec = config.pingIntervalS;
        websocket_cfg.disable_auto_reconnect = true;
        websocket_cfg.keep_alive_enable = true;
        websocket_cfg.network_timeout_ms = 10000;
        websocket_cfg.task_stack = 4096;
        websocket_cfg.buffer_size = static_cast<int>(DOWNLINK_RX_BUFFER_SIZE);

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

} // namespace btspeaker
