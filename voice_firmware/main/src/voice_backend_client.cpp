#include "voice_backend_client.hpp"

#include "speaker_protocol.hpp"
#include "uplink_audio_format.hpp"
#include "uplink_audio_frame.hpp"
#include "voice_config.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

extern "C" {
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

namespace {
constexpr const char *kSpeakerId = "rufus-001";
constexpr const char *kNodeId = "korvo2-voice-ui";
constexpr uint32_t kMicSampleRateHz = 16000;
constexpr int kWsReconnectPriority = 5;
constexpr int kIoCore = 0;

void copyJsonString(char *dst, const size_t dst_len, const cJSON *root, const char *key, const char *fallback = "") {
    if (!dst || dst_len == 0) {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    const char *value = cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
    std::strncpy(dst, value, dst_len - 1);
    dst[dst_len - 1] = '\0';
}
}  // namespace

VoiceBackendClient::VoiceBackendClient() = default;

VoiceBackendClient::~VoiceBackendClient() {
    stop();
}

void VoiceBackendClient::setNowPlayingCallback(NowPlayingCallback callback) {
    nowPlayingCallback_ = std::move(callback);
}

void VoiceBackendClient::setWaveformCallback(WaveformCallback callback) {
    waveformCallback_ = std::move(callback);
}

void VoiceBackendClient::setSessionStateCallback(SessionStateCallback callback) {
    sessionStateCallback_ = std::move(callback);
}

void VoiceBackendClient::setConnectionCallback(ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

bool VoiceBackendClient::isConnected() const {
    return canSend();
}

esp_err_t VoiceBackendClient::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return ESP_OK;
    }
    startReconnectLoop();
    return ESP_OK;
}

esp_err_t VoiceBackendClient::connectOnce() {
    if (websocketClient_ && !clientRunning_.load(std::memory_order_acquire)) {
        destroyClient();
    }

    connectInProgress_.store(true, std::memory_order_release);
    esp_err_t ret = createClient();
    if (ret != ESP_OK) {
        connectInProgress_.store(false, std::memory_order_release);
        return ret;
    }
    ret = esp_websocket_client_start(websocketClient_);
    if (ret != ESP_OK) {
        destroyClient();
        connectInProgress_.store(false, std::memory_order_release);
        return ret;
    }
    clientRunning_.store(true, std::memory_order_release);
    return ESP_OK;
}

void VoiceBackendClient::stop() {
    running_.store(false, std::memory_order_release);
    stopReconnectLoop();
    destroyClient();
}

esp_err_t VoiceBackendClient::createClient() {
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
        return ESP_FAIL;
    }
    return esp_websocket_register_events(websocketClient_, WEBSOCKET_EVENT_ANY, websocketEventHandler, this);
}

void VoiceBackendClient::destroyClient() {
    if (!websocketClient_ || destroying_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    // esp_websocket_client_destroy() stops the task when needed; avoid a separate stop()
    // call here to prevent "Client was not started" from double-stop.
    esp_websocket_client_destroy(websocketClient_);
    websocketClient_ = nullptr;
    connected_.store(false, std::memory_order_release);
    helloSent_.store(false, std::memory_order_release);
    clientRunning_.store(false, std::memory_order_release);
    destroying_.store(false, std::memory_order_release);
}

void VoiceBackendClient::websocketEventHandler(void *handlerArgs,
                                               esp_event_base_t,
                                               int32_t eventId,
                                               void *eventData) {
    auto *client = static_cast<VoiceBackendClient *>(handlerArgs);
    auto *data = eventId == WEBSOCKET_EVENT_DATA
                 ? static_cast<esp_websocket_event_data_t *>(eventData)
                 : nullptr;
    client->handleWebsocketEvent(static_cast<esp_websocket_event_id_t>(eventId), data);
}

void VoiceBackendClient::handleWebsocketEvent(esp_websocket_event_id_t eventId,
                                              esp_websocket_event_data_t *eventData) {
    switch (eventId) {
        case WEBSOCKET_EVENT_CONNECTED:
            recordConnectSuccess();
            signalConnectOutcome(CONNECT_OUTCOME_SUCCESS);
            connectInProgress_.store(false, std::memory_order_release);
            ESP_LOGI(TAG, "[NODE] transport connected speaker_id=%s node_id=%s role=%s board_profile=%s",
                     kSpeakerId,
                     kNodeId,
                     speaker_protocol::kRoleVoiceUiNode,
                     speaker_protocol::kBoardEsp32S3Korvo2Lcd);
            if (sendHello() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send hello; session will not accept mic uplink");
                signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
                break;
            }
            helloSent_.store(true, std::memory_order_release);
            connected_.store(true, std::memory_order_release);
            if (connectionCallback_) {
                connectionCallback_(true);
            }
            ESP_LOGI(TAG, "[NODE] hello sent for node_id=%s", kNodeId);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            if (!connected_.load(std::memory_order_acquire)) {
                signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
            }
            connected_.store(false, std::memory_order_release);
            helloSent_.store(false, std::memory_order_release);
            clientRunning_.store(false, std::memory_order_release);
            connectInProgress_.store(false, std::memory_order_release);
            textBuffer_.clear();
            if (connectionCallback_) {
                connectionCallback_(false);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WebSocket error: %s",
                     (eventData && eventData->data_ptr && eventData->data_len > 0)
                             ? eventData->data_ptr
                             : "unknown");
            if (!connected_.load(std::memory_order_acquire)) {
                signalConnectOutcome(CONNECT_OUTCOME_FAILURE);
            }
            connected_.store(false, std::memory_order_release);
            helloSent_.store(false, std::memory_order_release);
            clientRunning_.store(false, std::memory_order_release);
            connectInProgress_.store(false, std::memory_order_release);
            textBuffer_.clear();
            if (connectionCallback_) {
                connectionCallback_(false);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (eventData && eventData->op_code != 0x2 && eventData->data_ptr && eventData->data_len > 0) {
                if (eventData->payload_offset == 0) {
                    textBuffer_.clear();
                    textBuffer_.reserve(std::min<size_t>(eventData->payload_len, RX_MAX_PAYLOAD_BYTES));
                }
                textBuffer_.append(eventData->data_ptr, static_cast<size_t>(eventData->data_len));
                if (eventData->payload_offset + eventData->data_len >= eventData->payload_len) {
                    handleText(textBuffer_.data(), textBuffer_.size());
                    textBuffer_.clear();
                }
            }
            break;
        default:
            break;
    }
}

bool VoiceBackendClient::isTransportReady() const {
    return connected_.load(std::memory_order_acquire) &&
           helloSent_.load(std::memory_order_acquire) &&
           clientRunning_.load(std::memory_order_acquire) &&
           websocketClient_ != nullptr &&
           esp_websocket_client_is_connected(websocketClient_);
}

bool VoiceBackendClient::canSend() const {
    return helloSent_.load(std::memory_order_acquire) &&
           isTransportReady() &&
           esp_websocket_client_is_connected(websocketClient_);
}

uint32_t VoiceBackendClient::computeReconnectBackoffMs() const {
    uint32_t delay_ms = CONFIG_WS_RECONNECT_BACKOFF_INITIAL_MS;
    const uint32_t failures = consecutiveFailures_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < failures && delay_ms < CONFIG_WS_RECONNECT_BACKOFF_MAX_MS; ++i) {
        const uint32_t doubled = delay_ms * 2;
        delay_ms = doubled > CONFIG_WS_RECONNECT_BACKOFF_MAX_MS ? CONFIG_WS_RECONNECT_BACKOFF_MAX_MS : doubled;
    }
    return delay_ms;
}

uint32_t VoiceBackendClient::beginConnectAttempt() {
    const uint32_t generation = connectGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    connectOutcome_.store(CONNECT_OUTCOME_PENDING, std::memory_order_release);
    connectOutcomeGeneration_.store(0, std::memory_order_release);
    return generation;
}

void VoiceBackendClient::signalConnectOutcome(const uint32_t outcome) {
    const uint32_t generation = connectGeneration_.load(std::memory_order_acquire);
    connectOutcome_.store(outcome, std::memory_order_release);
    connectOutcomeGeneration_.store(generation, std::memory_order_release);
}

uint32_t VoiceBackendClient::waitForConnectOutcome(const uint32_t generation, const int timeout_ms) {
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

void VoiceBackendClient::recordConnectSuccess() {
    consecutiveFailures_.store(0, std::memory_order_relaxed);
}

void VoiceBackendClient::recordConnectFailure() {
    consecutiveFailures_.fetch_add(1, std::memory_order_relaxed);
}

void VoiceBackendClient::startReconnectLoop() {
    if (reconnectTaskHandle_ != nullptr) {
        return;
    }
    reconnectShutdown_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(reconnectLoopTask, "voice_ws_reconnect", 4096, this,
                                kWsReconnectPriority, &reconnectTaskHandle_, kIoCore) != pdPASS) {
        reconnectTaskHandle_ = nullptr;
        ESP_LOGE(TAG, "Failed to start WebSocket reconnect loop task");
    } else {
        ESP_LOGI(TAG, "WebSocket reconnect loop started (backoff %d-%d ms)",
                 CONFIG_WS_RECONNECT_BACKOFF_INITIAL_MS,
                 CONFIG_WS_RECONNECT_BACKOFF_MAX_MS);
    }
}

void VoiceBackendClient::stopReconnectLoop() {
    reconnectShutdown_.store(true, std::memory_order_release);
    for (int i = 0; i < 100 && reconnectTaskHandle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (reconnectTaskHandle_ != nullptr) {
        vTaskDelete(reconnectTaskHandle_);
        reconnectTaskHandle_ = nullptr;
    }
}

void VoiceBackendClient::reconnectLoopTask(void *arg) {
    auto *client = static_cast<VoiceBackendClient *>(arg);
    while (!client->reconnectShutdown_.load(std::memory_order_acquire)) {
        if (client->websocketClient_ &&
            !esp_websocket_client_is_connected(client->websocketClient_) &&
            (client->connected_.load(std::memory_order_acquire) ||
             client->helloSent_.load(std::memory_order_acquire) ||
             client->clientRunning_.load(std::memory_order_acquire))) {
            ESP_LOGW(TAG, "WebSocket transport down; forcing reconnect");
            client->destroyClient();
        }

        if (client->websocketClient_ &&
            esp_websocket_client_is_connected(client->websocketClient_) &&
            !client->helloSent_.load(std::memory_order_acquire) &&
            !client->connectInProgress_.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "WebSocket connected without hello; sending hello");
            if (client->sendHello() == ESP_OK) {
                client->helloSent_.store(true, std::memory_order_release);
                client->connected_.store(true, std::memory_order_release);
                if (client->connectionCallback_) {
                    client->connectionCallback_(true);
                }
                client->recordConnectSuccess();
            } else {
                ESP_LOGE(TAG, "Hello resend failed; reconnecting");
                client->destroyClient();
            }
        }

        if (client->canSend()) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_LOOP_IDLE_MS));
            continue;
        }

        if (client->connectInProgress_.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DEBOUNCE_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DEBOUNCE_MS));
        if (client->reconnectShutdown_.load(std::memory_order_acquire) ||
            client->isTransportReady() ||
            client->connectInProgress_.load(std::memory_order_acquire)) {
            continue;
        }

        const uint32_t backoff_ms = client->computeReconnectBackoffMs();
        const uint32_t attempt = client->consecutiveFailures_.load(std::memory_order_relaxed) + 1;
        ESP_LOGI(TAG, "WebSocket reconnect in %lu ms (attempt %lu)",
                 static_cast<unsigned long>(backoff_ms),
                 static_cast<unsigned long>(attempt));
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));

        if (client->reconnectShutdown_.load(std::memory_order_acquire) ||
            client->canSend() ||
            client->connectInProgress_.load(std::memory_order_acquire)) {
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
        client->connectInProgress_.store(false, std::memory_order_release);
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

void VoiceBackendClient::handleText(const char *data, const size_t length) {
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (std::strcmp(type->valuestring, speaker_protocol::kMessageUiNowPlaying) == 0 && nowPlayingCallback_) {
        NowPlayingUiState state{};
        copyJsonString(state.source, sizeof(state.source), root, "source", "unknown");
        copyJsonString(state.playback_state, sizeof(state.playback_state), root, "playback_state", "unknown");
        copyJsonString(state.title, sizeof(state.title), root, "title", "");
        copyJsonString(state.artist, sizeof(state.artist), root, "artist", "");
        copyJsonString(state.album, sizeof(state.album), root, "album", "");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
        const cJSON *position = cJSON_GetObjectItemCaseSensitive(root, "position_ms");
        if (cJSON_IsNumber(duration)) state.duration_ms = static_cast<uint32_t>(duration->valuedouble);
        if (cJSON_IsNumber(position)) state.position_ms = static_cast<uint32_t>(position->valuedouble);
        nowPlayingCallback_(state);
    } else if (std::strcmp(type->valuestring, speaker_protocol::kMessageUiWaveform) == 0 && waveformCallback_) {
        WaveformUiState state{};
        const cJSON *buckets = cJSON_GetObjectItemCaseSensitive(root, "buckets");
        if (cJSON_IsArray(buckets)) {
            const int count = std::min(cJSON_GetArraySize(buckets), static_cast<int>(WaveformUiState::kMaxBuckets));
            for (int i = 0; i < count; ++i) {
                const cJSON *item = cJSON_GetArrayItem(buckets, i);
                if (cJSON_IsNumber(item)) {
                    state.buckets[static_cast<size_t>(i)] = static_cast<float>(item->valuedouble);
                    state.bucket_count = static_cast<size_t>(i + 1);
                }
            }
        }
        waveformCallback_(state);
    } else if (std::strcmp(type->valuestring, speaker_protocol::kMessageStateChange) == 0 && sessionStateCallback_) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(state)) {
            sessionStateCallback_(state->valuestring);
        }
    }
    cJSON_Delete(root);
}

esp_err_t VoiceBackendClient::sendTextOnWire(const char *text) {
    if (!websocketClient_ || !text || !esp_websocket_client_is_connected(websocketClient_)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int len = static_cast<int>(std::strlen(text));
    const int sent = esp_websocket_client_send_text(websocketClient_, text, len, pdMS_TO_TICKS(250));
    return sent == len ? ESP_OK : ESP_FAIL;
}

esp_err_t VoiceBackendClient::sendText(const char *text) {
    if (!canSend() || !text) {
        return ESP_ERR_INVALID_STATE;
    }
    return sendTextOnWire(text);
}

esp_err_t VoiceBackendClient::sendHello() {
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
    cJSON_AddStringToObject(root, "node_role", speaker_protocol::kRoleVoiceUiNode);
    cJSON_AddStringToObject(root, "board_profile", speaker_protocol::kBoardEsp32S3Korvo2Lcd);
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityMicUplink));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityDisplay));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityNowPlayingUi));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString(speaker_protocol::kCapabilityWaveformUi));
    cJSON_AddItemToObject(root, "capabilities", capabilities);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ret = sendTextOnWire(json);
    cJSON_free(json);
    return ret;
}

esp_err_t VoiceBackendClient::sendMicFrame(const int16_t *samples,
                                           const uint32_t sample_count,
                                           const uint32_t sequence,
                                           const int64_t timestamp_us) {
    if (!canSend() || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t payload_bytes = static_cast<size_t>(sample_count) * sizeof(int16_t);
    const size_t total = 1 + kUplinkFramedHeaderSize + payload_bytes;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    buffer[0] = kUplinkFormatFramed;
    auto *header = reinterpret_cast<UplinkAudioFrameHeader *>(buffer + 1);
    header->frame_type = static_cast<uint8_t>(AudioFrameType::Mic);
    header->sample_format = static_cast<uint8_t>(UplinkSampleFormat::Int16Pcm);
    header->channel_count = 1;
    header->sample_rate = kMicSampleRateHz;
    header->sequence = sequence;
    header->timestamp_us = timestamp_us;
    header->num_samples = sample_count;
    std::memcpy(buffer + 1 + kUplinkFramedHeaderSize, samples, payload_bytes);
    const int sent = esp_websocket_client_send_bin(
            websocketClient_,
            reinterpret_cast<const char *>(buffer),
            static_cast<int>(total),
            pdMS_TO_TICKS(250));
    heap_caps_free(buffer);
    if (sent != static_cast<int>(total)) {
        connected_.store(false, std::memory_order_release);
        helloSent_.store(false, std::memory_order_release);
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace btspeaker
