#include "playback_backend_client.hpp"

#include "speaker_protocol.hpp"
#include "task_affinity.hpp"
#include "uplink_audio_format.hpp"
#include "uplink_audio_frame.hpp"
#include "voice_config.hpp"
#include <cstring>
#include <utility>

extern "C" {
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

namespace btspeaker {

namespace {
constexpr const char *kSpeakerId = "rufus-001";
constexpr const char *kNodeId = "wrover-main";
}  // namespace

PlaybackBackendClient::PlaybackBackendClient() = default;

PlaybackBackendClient::~PlaybackBackendClient() {
    stop();
}

void PlaybackBackendClient::setAudioDownlinkCallback(AudioDownlinkCallback callback) {
    audioDownlinkCallback_ = std::move(callback);
}

void PlaybackBackendClient::setIntentCallback(IntentCallback callback) {
    intentCallback_ = std::move(callback);
}

bool PlaybackBackendClient::isConnected() const {
    return isTransportReady();
}

esp_err_t PlaybackBackendClient::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return ESP_OK;
    }
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    const esp_err_t ref_ret = startPlaybackRefWorker();
    if (ref_ret != ESP_OK) {
        running_.store(false, std::memory_order_release);
        return ref_ret;
    }
#endif
    startReconnectLoop();
    return ESP_OK;
}

esp_err_t PlaybackBackendClient::createClient() {
    if (websocketClient_) {
        return ESP_OK;
    }
    esp_websocket_client_config_t config = {};
    config.uri = CONFIG_WEBSOCKET_SERVER_URL;
    config.buffer_size = 4096;
    config.network_timeout_ms = 10000;
    config.reconnect_timeout_ms = 1000;
    config.disable_auto_reconnect = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = CONFIG_WS_KEEPALIVE_INTERVAL_S;
    config.ping_interval_sec = CONFIG_WS_PING_INTERVAL_S;
    config.task_stack = 4096;
    websocketClient_ = esp_websocket_client_init(&config);
    if (!websocketClient_) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return ESP_FAIL;
    }
    return esp_websocket_register_events(websocketClient_,
                                         WEBSOCKET_EVENT_ANY,
                                         websocketEventHandler,
                                         this);
}

esp_err_t PlaybackBackendClient::connectOnce() {
    esp_err_t ret = createClient();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_websocket_client_start(websocketClient_);
    if (ret != ESP_OK) {
        destroyClient();
        return ret;
    }
    clientRunning_.store(true, std::memory_order_release);
    return ESP_OK;
}

void PlaybackBackendClient::stop() {
    running_.store(false, std::memory_order_release);
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    stopPlaybackRefWorker();
#endif
    stopReconnectLoop();
    destroyClient();
    connected_.store(false, std::memory_order_release);
}

void PlaybackBackendClient::destroyClient() {
    if (!websocketClient_ || destroying_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (clientRunning_.load(std::memory_order_acquire)) {
        esp_websocket_client_stop(websocketClient_);
        vTaskDelay(pdMS_TO_TICKS(300));
        clientRunning_.store(false, std::memory_order_release);
    }
    esp_websocket_client_destroy(websocketClient_);
    websocketClient_ = nullptr;
    connected_.store(false, std::memory_order_release);
    destroying_.store(false, std::memory_order_release);
}

void PlaybackBackendClient::websocketEventHandler(void *handlerArgs,
                                                  esp_event_base_t,
                                                  int32_t eventId,
                                                  void *eventData) {
    auto *client = static_cast<PlaybackBackendClient *>(handlerArgs);
    auto *data = eventId == WEBSOCKET_EVENT_DATA
                 ? static_cast<esp_websocket_event_data_t *>(eventData)
                 : nullptr;
    client->handleWebsocketEvent(static_cast<esp_websocket_event_id_t>(eventId), data);
}

void PlaybackBackendClient::handleWebsocketEvent(esp_websocket_event_id_t eventId,
                                                 esp_websocket_event_data_t *eventData) {
    switch (eventId) {
        case WEBSOCKET_EVENT_CONNECTED:
            recordConnectSuccess();
            signalConnectOutcome(CONNECT_OUTCOME_SUCCESS);
            connected_.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "[NODE] connected speaker_id=%s node_id=%s role=%s board_profile=%s",
                     kSpeakerId,
                     kNodeId,
                     speaker_protocol::kRolePlaybackNode,
                     speaker_protocol::kBoardEsp32WroverPlayback);
            (void) sendHello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            if (!connected_.load(std::memory_order_acquire)) {
                signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
            }
            connected_.store(false, std::memory_order_release);
            clientRunning_.store(false, std::memory_order_release);
            binaryBuffer_.clear();
            textBuffer_.clear();
            break;
        case WEBSOCKET_EVENT_ERROR:
            if (!connected_.load(std::memory_order_acquire)) {
                signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
            }
            connected_.store(false, std::memory_order_release);
            clientRunning_.store(false, std::memory_order_release);
            binaryBuffer_.clear();
            textBuffer_.clear();
            break;
        case WEBSOCKET_EVENT_DATA:
            if (eventData) {
                handleDownlinkFragment(eventData, eventData->op_code == 0x2);
            }
            break;
        default:
            break;
    }
}

bool PlaybackBackendClient::isTransportReady() const {
    return connected_.load(std::memory_order_acquire) &&
           websocketClient_ != nullptr &&
           esp_websocket_client_is_connected(websocketClient_);
}

uint32_t PlaybackBackendClient::computeReconnectBackoffMs() const {
    uint32_t delay_ms = CONFIG_WS_RECONNECT_BACKOFF_INITIAL_MS;
    const uint32_t failures = consecutiveFailures_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < failures && delay_ms < CONFIG_WS_RECONNECT_BACKOFF_MAX_MS; ++i) {
        const uint32_t doubled = delay_ms * 2;
        delay_ms = doubled > CONFIG_WS_RECONNECT_BACKOFF_MAX_MS ? CONFIG_WS_RECONNECT_BACKOFF_MAX_MS : doubled;
    }
    return delay_ms;
}

uint32_t PlaybackBackendClient::beginConnectAttempt() {
    const uint32_t generation = connectGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    connectOutcome_.store(CONNECT_OUTCOME_PENDING, std::memory_order_release);
    connectOutcomeGeneration_.store(0, std::memory_order_release);
    return generation;
}

void PlaybackBackendClient::signalConnectOutcome(const uint32_t outcome) {
    const uint32_t generation = connectGeneration_.load(std::memory_order_acquire);
    connectOutcome_.store(outcome, std::memory_order_release);
    connectOutcomeGeneration_.store(generation, std::memory_order_release);
}

uint32_t PlaybackBackendClient::waitForConnectOutcome(const uint32_t generation, const int timeout_ms) {
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

void PlaybackBackendClient::recordConnectSuccess() {
    consecutiveFailures_.store(0, std::memory_order_relaxed);
}

void PlaybackBackendClient::recordConnectFailure() {
    consecutiveFailures_.fetch_add(1, std::memory_order_relaxed);
}

void PlaybackBackendClient::startReconnectLoop() {
    if (reconnectTaskHandle_ != nullptr) {
        return;
    }
    reconnectShutdown_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(reconnectLoopTask, "pb_ws_reconnect", 4096, this,
                                kWsReconnectPriority, &reconnectTaskHandle_, kIoCore) != pdPASS) {
        reconnectTaskHandle_ = nullptr;
        ESP_LOGE(TAG, "Failed to start WebSocket reconnect loop task");
    } else {
        ESP_LOGI(TAG, "WebSocket reconnect loop started (backoff %d-%d ms)",
                 CONFIG_WS_RECONNECT_BACKOFF_INITIAL_MS,
                 CONFIG_WS_RECONNECT_BACKOFF_MAX_MS);
    }
}

void PlaybackBackendClient::stopReconnectLoop() {
    reconnectShutdown_.store(true, std::memory_order_release);
    if (reconnectTaskHandle_ != nullptr) {
        vTaskDelete(reconnectTaskHandle_);
        reconnectTaskHandle_ = nullptr;
    }
}

void PlaybackBackendClient::reconnectLoopTask(void *arg) {
    auto *client = static_cast<PlaybackBackendClient *>(arg);
    while (!client->reconnectShutdown_.load(std::memory_order_acquire)) {
        if (client->isTransportReady()) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_LOOP_IDLE_MS));
            continue;
        }

        const uint32_t backoff_ms = client->computeReconnectBackoffMs();
        const uint32_t attempt = client->consecutiveFailures_.load(std::memory_order_relaxed) + 1;
        ESP_LOGI(TAG, "WebSocket reconnect in %lu ms (attempt %lu)",
                 static_cast<unsigned long>(backoff_ms),
                 static_cast<unsigned long>(attempt));
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));

        if (client->reconnectShutdown_.load(std::memory_order_acquire) || client->isTransportReady()) {
            continue;
        }

        if (client->websocketClient_) {
            client->destroyClient();
        }

        const uint32_t generation = client->beginConnectAttempt();
        const esp_err_t ret = client->connectOnce();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket connect start failed: %s", esp_err_to_name(ret));
            client->connected_.store(false, std::memory_order_release);
            client->recordConnectFailure();
            continue;
        }

        const uint32_t outcome = client->waitForConnectOutcome(generation, RECONNECT_CONNECT_WAIT_MS);
        if (outcome == CONNECT_OUTCOME_SUCCESS) {
            ESP_LOGI(TAG, "WebSocket connected to %s", CONFIG_WEBSOCKET_SERVER_URL);
            client->recordConnectSuccess();
            continue;
        }

        ESP_LOGW(TAG, "WebSocket connect attempt failed (outcome=%lu)",
                 static_cast<unsigned long>(outcome));
        if (client->websocketClient_) {
            client->destroyClient();
        }
        client->connected_.store(false, std::memory_order_release);
        client->recordConnectFailure();
    }

    client->reconnectTaskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

void PlaybackBackendClient::handleDownlinkFragment(esp_websocket_event_data_t *eventData, const bool is_binary) {
    if (!eventData || !eventData->data_ptr || eventData->data_len <= 0 || eventData->payload_len <= 0) {
        return;
    }
    if (static_cast<size_t>(eventData->payload_len) > RX_MAX_PAYLOAD_BYTES) {
        ESP_LOGW(TAG, "Ignoring oversized downlink payload (%d bytes)", eventData->payload_len);
        return;
    }
    auto &buffer = is_binary ? binaryBuffer_ : textBuffer_;
    if (eventData->payload_offset == 0) {
        buffer.clear();
        buffer.reserve(static_cast<size_t>(eventData->payload_len));
    }
    buffer.append(eventData->data_ptr, static_cast<size_t>(eventData->data_len));
    if (eventData->payload_offset + eventData->data_len < eventData->payload_len) {
        return;
    }

    if (is_binary) {
        if (audioDownlinkCallback_) {
            audioDownlinkCallback_(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size());
        }
    } else {
        cJSON *root = cJSON_ParseWithLength(buffer.data(), buffer.size());
        if (root) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
            if (cJSON_IsString(type) && std::strcmp(type->valuestring, speaker_protocol::kMessageIntent) == 0) {
                const cJSON *intent = cJSON_GetObjectItemCaseSensitive(root, "intent");
                const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
                if (cJSON_IsString(intent) && cJSON_IsString(data) && intentCallback_) {
                    intentCallback_(intent->valuestring, data->valuestring);
                }
            }
            cJSON_Delete(root);
        }
    }
    buffer.clear();
}

esp_err_t PlaybackBackendClient::sendText(const char *text) {
    if (!isConnected() || !websocketClient_ || !text) {
        return ESP_ERR_INVALID_STATE;
    }
    const int len = static_cast<int>(std::strlen(text));
    const int ret = esp_websocket_client_send_text(websocketClient_, text, len, pdMS_TO_TICKS(250));
    return ret == len ? ESP_OK : ESP_FAIL;
}

esp_err_t PlaybackBackendClient::sendHello() {
    cJSON *root = cJSON_CreateObject();
    cJSON *capabilities = cJSON_CreateArray();
    if (!root || !capabilities) {
        cJSON_Delete(root);
        cJSON_Delete(capabilities);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", speaker_protocol::kMessageHello);
    cJSON_AddNumberToObject(root, "protocol_version", speaker_protocol::kProtocolVersion);
    cJSON_AddStringToObject(root, "speaker_id", kSpeakerId);
    cJSON_AddStringToObject(root, "node_id", kNodeId);
    cJSON_AddStringToObject(root, "node_role", speaker_protocol::kRolePlaybackNode);
    cJSON_AddStringToObject(root, "board_profile", speaker_protocol::kBoardEsp32WroverPlayback);
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilitySpotify));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityBluetoothA2dp));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilitySpeakerOutput));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityLlmAudioDownlink));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityPlaybackRef));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityPlaybackState));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityWaveformSource));
    cJSON_AddItemToObject(root, "capabilities", capabilities);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = sendText(json);
    cJSON_free(json);
    return ret;
}

esp_err_t PlaybackBackendClient::sendPlaybackRef(const int16_t *pcm,
                                                 const uint32_t num_samples,
                                                 const uint32_t sample_rate_hz,
                                                 const uint16_t channel_count,
                                                 const uint32_t sequence,
                                                 const int64_t timestamp_us) {
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    if (!isConnected() || !pcm || num_samples == 0 || channel_count != 1 ||
        num_samples > kPlaybackRefFrameSamples || !playbackRefQueue_ || !playbackRefFreeQueue_) {
        return ESP_ERR_INVALID_STATE;
    }

    PlaybackRefFrame *frame = nullptr;
    if (xQueueReceive(playbackRefFreeQueue_, &frame, 0) != pdTRUE || !frame) {
        return ESP_ERR_NO_MEM;
    }

    std::memcpy(frame->pcm, pcm, static_cast<size_t>(num_samples) * sizeof(int16_t));
    frame->num_samples = num_samples;
    frame->sample_rate_hz = sample_rate_hz;
    frame->channel_count = channel_count;
    frame->sequence = sequence;
    frame->timestamp_us = timestamp_us;

    if (xQueueSend(playbackRefQueue_, &frame, 0) != pdTRUE) {
        (void) xQueueSend(playbackRefFreeQueue_, &frame, 0);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
#else
    (void) pcm;
    (void) num_samples;
    (void) sample_rate_hz;
    (void) channel_count;
    (void) sequence;
    (void) timestamp_us;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
esp_err_t PlaybackBackendClient::sendPlaybackRefNow(const PlaybackRefFrame &frame) {
    if (!isConnected() || !websocketClient_ || frame.num_samples == 0 || frame.channel_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t payload_bytes = static_cast<size_t>(frame.num_samples) *
                                 frame.channel_count * sizeof(int16_t);
    const size_t total = 1 + kUplinkFramedHeaderSize + payload_bytes;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    buffer[0] = kUplinkFormatFramed;
    auto *header = reinterpret_cast<UplinkAudioFrameHeader *>(buffer + 1);
    header->frame_type = static_cast<uint8_t>(AudioFrameType::PlaybackRef);
    header->sample_format = static_cast<uint8_t>(UplinkSampleFormat::Int16Pcm);
    header->channel_count = frame.channel_count;
    header->sample_rate = frame.sample_rate_hz;
    header->sequence = frame.sequence;
    header->timestamp_us = frame.timestamp_us;
    header->num_samples = frame.num_samples;
    std::memcpy(buffer + 1 + kUplinkFramedHeaderSize, frame.pcm, payload_bytes);
    const int sent = esp_websocket_client_send_bin(
            websocketClient_,
            reinterpret_cast<const char *>(buffer),
            static_cast<int>(total),
            pdMS_TO_TICKS(PLAYBACK_REF_SEND_TIMEOUT_MS));
    heap_caps_free(buffer);
    if (sent == static_cast<int>(total)) {
        return ESP_OK;
    }
    if (sent < 0 || sent > 0) {
        connected_.store(false, std::memory_order_release);
        clientRunning_.store(false, std::memory_order_release);
    }
    return ESP_FAIL;
}

esp_err_t PlaybackBackendClient::startPlaybackRefWorker() {
    if (playbackRefTaskHandle_) {
        return ESP_OK;
    }

    playbackRefQueue_ = xQueueCreate(PLAYBACK_REF_QUEUE_DEPTH, sizeof(PlaybackRefFrame *));
    playbackRefFreeQueue_ = xQueueCreate(PLAYBACK_REF_QUEUE_DEPTH, sizeof(PlaybackRefFrame *));
    if (!playbackRefQueue_ || !playbackRefFreeQueue_) {
        stopPlaybackRefWorker();
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < PLAYBACK_REF_QUEUE_DEPTH; ++i) {
        PlaybackRefFrame *frame = &playbackRefPool_[i];
        if (xQueueSend(playbackRefFreeQueue_, &frame, 0) != pdTRUE) {
            stopPlaybackRefWorker();
            return ESP_FAIL;
        }
    }

    playbackRefWorkerShutdown_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(playbackRefWorkerTask, "pb_ref_tx", 4096, this,
                                kMicUplinkPriority, &playbackRefTaskHandle_, kIoCore) != pdPASS) {
        playbackRefTaskHandle_ = nullptr;
        stopPlaybackRefWorker();
        return ESP_FAIL;
    }
    return ESP_OK;
}

void PlaybackBackendClient::stopPlaybackRefWorker() {
    playbackRefWorkerShutdown_.store(true, std::memory_order_release);
    if (playbackRefTaskHandle_) {
        PlaybackRefFrame *shutdown = nullptr;
        (void) xQueueSend(playbackRefQueue_, &shutdown, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (playbackRefTaskHandle_) {
            vTaskDelete(playbackRefTaskHandle_);
            playbackRefTaskHandle_ = nullptr;
        }
    }
    if (playbackRefQueue_) {
        vQueueDelete(playbackRefQueue_);
        playbackRefQueue_ = nullptr;
    }
    if (playbackRefFreeQueue_) {
        vQueueDelete(playbackRefFreeQueue_);
        playbackRefFreeQueue_ = nullptr;
    }
}

void PlaybackBackendClient::playbackRefWorkerTask(void *arg) {
    auto *client = static_cast<PlaybackBackendClient *>(arg);
    client->runPlaybackRefWorker();
    client->playbackRefTaskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

void PlaybackBackendClient::runPlaybackRefWorker() {
    while (!playbackRefWorkerShutdown_.load(std::memory_order_acquire)) {
        PlaybackRefFrame *frame = nullptr;
        if (xQueueReceive(playbackRefQueue_, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!frame) {
            break;
        }
        (void) sendPlaybackRefNow(*frame);
        (void) xQueueSend(playbackRefFreeQueue_, &frame, 0);
    }
}
#endif

esp_err_t PlaybackBackendClient::publishPlaybackState(const char *source,
                                                      const char *state,
                                                      const char *title,
                                                      const char *artist,
                                                      const char *album,
                                                      const int duration_ms,
                                                      const int position_ms) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", speaker_protocol::kMessagePlaybackState);
    cJSON_AddStringToObject(root, "speaker_id", kSpeakerId);
    cJSON_AddStringToObject(root, "source", source ? source : "unknown");
    cJSON_AddStringToObject(root, "playback_state", state ? state : "unknown");
    if (title) cJSON_AddStringToObject(root, "title", title);
    if (artist) cJSON_AddStringToObject(root, "artist", artist);
    if (album) cJSON_AddStringToObject(root, "album", album);
    if (duration_ms >= 0) cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
    if (position_ms >= 0) cJSON_AddNumberToObject(root, "position_ms", position_ms);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = sendText(json);
    cJSON_free(json);
    return ret;
}

}  // namespace btspeaker
