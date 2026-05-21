#include "../include/voice_assistant_manager.hpp"
#include "../include/task_affinity.hpp"
#include "../include/microphone_manager.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/audio_source_manager.hpp"
#include "diagnostics_config.hpp"
#include <cstring>
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
}

namespace btspeaker {

    namespace {

        void *allocateInboundPayload(size_t size) {
            if (size == 0) {
                return nullptr;
            }
            void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!ptr) {
                ptr = malloc(size);
            }
            return ptr;
        }

        void freeInboundPayloadPtr(void *ptr) {
            if (!ptr) {
                return;
            }
            heap_caps_free(ptr);
        }

    } // namespace

    const char *VoiceAssistantManager::voiceStateName(const VoiceAssistantState voice_state) {
        switch (voice_state) {
            case VoiceAssistantState::IDLE:
                return "IDLE";
            case VoiceAssistantState::LISTENING:
                return "LISTENING";
            case VoiceAssistantState::PROCESSING_INTENT:
                return "PROCESSING_INTENT";
            case VoiceAssistantState::STREAMING_RESPONSE:
                return "STREAMING_RESPONSE";
            case VoiceAssistantState::HANDLING_COMMAND:
                return "HANDLING_COMMAND";
            case VoiceAssistantState::WAITING_FOR_USER:
                return "WAITING_FOR_USER";
            default:
                return "UNKNOWN";
        }
    }

    const char *VoiceAssistantManager::audioSourceName(const AudioSource source) {
        switch (source) {
            case AudioSource::NONE:
                return "NONE";
            case AudioSource::VOICE_RESPONSE:
                return "VOICE_RESPONSE";
            case AudioSource::BLUETOOTH:
                return "BLUETOOTH";
            case AudioSource::SPOTIFY:
                return "SPOTIFY";
            default:
                return "UNKNOWN";
        }
    }

    void VoiceAssistantManager::logUpstreamDiagnostics(const int64_t now_us,
                                                         const uint32_t chunks_received,
                                                         const uint32_t chunks_sent,
                                                         const uint32_t chunks_skipped_state,
                                                         const uint32_t chunks_skipped_disconnected,
                                                         const uint32_t chunks_skipped_spotify_protect,
                                                         const uint32_t chunks_send_failed,
                                                         const uint64_t bytes_sent,
                                                         const uint64_t sum_send_us,
                                                         const uint32_t max_send_us) const {
        if (!diagnostics_enabled) {
            return;
        }

        const uint32_t avg_send_us = chunks_sent > 0
                                     ? static_cast<uint32_t>(sum_send_us / chunks_sent)
                                     : 0;
        const uint32_t effective_hz = (UPSTREAM_DIAG_INTERVAL_MS > 0 && bytes_sent > 0)
                                      ? static_cast<uint32_t>(
                                              (bytes_sent * 1000ULL) /
                                              (static_cast<uint64_t>(UPSTREAM_DIAG_INTERVAL_MS) * 2ULL))
                                      : 0;

        ESP_LOGI(TAG,
                 "[UPSTREAM_DIAG] rx_chunks=%lu sent=%lu skip_state=%lu skip_ws=%lu skip_spotify=%lu send_fail=%lu "
                 "bytes=%lu ~%luHz voice=%s audio=%s "
                 "ws_send_us(avg=%lu max=%lu) dram=%lu",
                 static_cast<unsigned long>(chunks_received),
                 static_cast<unsigned long>(chunks_sent),
                 static_cast<unsigned long>(chunks_skipped_state),
                 static_cast<unsigned long>(chunks_skipped_disconnected),
                 static_cast<unsigned long>(chunks_skipped_spotify_protect),
                 static_cast<unsigned long>(chunks_send_failed),
                 static_cast<unsigned long>(bytes_sent),
                 static_cast<unsigned long>(effective_hz),
                 voiceStateName(state.load()),
                 audioSourceName(audioSourceManager.getCurrentSource()),
                 static_cast<unsigned long>(avg_send_us),
                 static_cast<unsigned long>(max_send_us),
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        (void) now_us;
    }

    VoiceAssistantManager::VoiceAssistantManager(AudioSourceManager &audioSourceManager)
            : state(VoiceAssistantState::IDLE),
              audioSourceManager(audioSourceManager) {
        inboundQueue_ = xQueueCreate(INBOUND_QUEUE_DEPTH, sizeof(InboundMessage));
        if (!inboundQueue_) {
            throw std::runtime_error("Failed to create voice inbound queue");
        }

        inboundStoppedSem_ = xSemaphoreCreateBinary();
        if (!inboundStoppedSem_) {
            vQueueDelete(inboundQueue_);
            inboundQueue_ = nullptr;
            throw std::runtime_error("Failed to create voice inbound shutdown semaphore");
        }

        inboundTaskStack_ = static_cast<StackType_t *>(heap_caps_malloc(
                INBOUND_TASK_STACK_WORDS * sizeof(StackType_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        inboundTaskBuffer_ = static_cast<StaticTask_t *>(heap_caps_malloc(
                sizeof(StaticTask_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (!inboundTaskStack_ || !inboundTaskBuffer_) {
            shutdownInboundWorker();
            throw std::runtime_error("Failed to allocate voice inbound worker stack");
        }

        inboundAccepting_.store(true);
        inboundTaskHandle_ = xTaskCreateStaticPinnedToCore(
                inboundWorkerTask,
                "voice_inbound",
                INBOUND_TASK_STACK_WORDS,
                this,
                kVoiceInboundPriority,
                inboundTaskStack_,
                inboundTaskBuffer_,
                kIoCore);

        if (!inboundTaskHandle_) {
            shutdownInboundWorker();
            throw std::runtime_error("Failed to start voice inbound worker task");
        }

        websocketClient = std::make_unique<WebSocketVoiceClient>(*this);
        websocketClient->init();

        microphoneManager = std::make_unique<MicrophoneManager>();
        microphoneManager->init();
        microphoneManager->setAudioCallback(
                [this](const uint8_t *data, size_t length) {
                    onAudioInput(data, length);
                }
        );

        setVoiceAssistantState(VoiceAssistantState::IDLE);
        ESP_LOGI(TAG, "VoiceAssistantManager initialized successfully");
    }

    VoiceAssistantManager::~VoiceAssistantManager() {
        inboundAccepting_.store(false);

        if (microphoneManager) {
            microphoneManager->shutdown();
        }

        if (websocketClient) {
            websocketClient->shutdown();
        }

        shutdownInboundWorker();
    }

    void VoiceAssistantManager::onNetworkLost() {
        if (websocketClient) {
            websocketClient->onNetworkLost();
        }
    }

    void VoiceAssistantManager::onNetworkAvailable() {
        if (websocketClient) {
            websocketClient->onNetworkAvailable();
        }
    }

    esp_err_t VoiceAssistantManager::start() {
        setVoiceAssistantState(VoiceAssistantState::LISTENING);

        esp_err_t ret = microphoneManager->startRecording();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start microphone recording: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Voice assistant started");
        return ESP_OK;
    }

    void VoiceAssistantManager::onAudioResponse(const uint8_t *data, size_t length) {
        if (!inboundAccepting_.load() || !data || length == 0) {
            return;
        }

        void *copy = allocateInboundPayload(length);
        if (!copy) {
            ESP_LOGE(TAG, "Failed to allocate audio inbound buffer (%u bytes)", static_cast<unsigned>(length));
            return;
        }

        memcpy(copy, data, length);

        InboundMessage message{
                .type = InboundMessageType::AUDIO_PCM,
                .length = static_cast<uint32_t>(length),
                .payload = copy,
        };

        if (!enqueueInboundMessage(std::move(message), INBOUND_AUDIO_ENQUEUE_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Dropped audio response chunk (%u bytes), inbound queue full",
                     static_cast<unsigned>(length));
            freeInboundPayloadPtr(copy);
        }
    }

    void VoiceAssistantManager::onTextData(const uint8_t *data, size_t length) {
        if (!inboundAccepting_.load() || !data || length == 0) {
            return;
        }

        if (length >= MAX_INBOUND_TEXT_BYTES) {
            ESP_LOGW(TAG, "Ignoring oversized text message (%u bytes)", static_cast<unsigned>(length));
            return;
        }

        const size_t copy_len = length + 1;
        auto *copy = static_cast<char *>(allocateInboundPayload(copy_len));
        if (!copy) {
            ESP_LOGE(TAG, "Failed to allocate text inbound buffer (%u bytes)", static_cast<unsigned>(copy_len));
            return;
        }

        memcpy(copy, data, length);
        copy[length] = '\0';

        InboundMessage message{
                .type = InboundMessageType::TEXT_JSON,
                .length = static_cast<uint32_t>(length),
                .payload = copy,
        };

        if (!enqueueInboundMessage(std::move(message), 0)) {
            ESP_LOGW(TAG, "Dropped text message (%u bytes), inbound queue full",
                     static_cast<unsigned>(length));
            freeInboundPayloadPtr(copy);
        }
    }

    bool VoiceAssistantManager::enqueueInboundMessage(InboundMessage &&message, TickType_t timeout_ms) {
        if (!inboundQueue_) {
            return false;
        }
        const TickType_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0;
        return xQueueSend(inboundQueue_, &message, ticks) == pdTRUE;
    }

    void VoiceAssistantManager::freeInboundPayload(InboundMessage &message) {
        if (message.type == InboundMessageType::SHUTDOWN) {
            return;
        }
        freeInboundPayloadPtr(message.payload);
        message.payload = nullptr;
        message.length = 0;
    }

    void VoiceAssistantManager::shutdownInboundWorker() {
        inboundAccepting_.store(false);

        if (inboundQueue_) {
            InboundMessage shutdown_message{
                    .type = InboundMessageType::SHUTDOWN,
                    .length = 0,
                    .payload = nullptr,
            };
            xQueueSend(inboundQueue_, &shutdown_message, pdMS_TO_TICKS(100));
        }

        if (inboundTaskHandle_ && inboundStoppedSem_) {
            xSemaphoreTake(inboundStoppedSem_, pdMS_TO_TICKS(5000));
            inboundTaskHandle_ = nullptr;
        }

        if (inboundQueue_) {
            InboundMessage pending{};
            while (xQueueReceive(inboundQueue_, &pending, 0) == pdTRUE) {
                freeInboundPayload(pending);
            }
            vQueueDelete(inboundQueue_);
            inboundQueue_ = nullptr;
        }

        if (inboundStoppedSem_) {
            vSemaphoreDelete(inboundStoppedSem_);
            inboundStoppedSem_ = nullptr;
        }

        if (inboundTaskStack_) {
            heap_caps_free(inboundTaskStack_);
            inboundTaskStack_ = nullptr;
        }

        if (inboundTaskBuffer_) {
            heap_caps_free(inboundTaskBuffer_);
            inboundTaskBuffer_ = nullptr;
        }
    }

    void VoiceAssistantManager::inboundWorkerTask(void *arg) {
        auto *self = static_cast<VoiceAssistantManager *>(arg);
        self->runInboundWorker();
        if (self->inboundStoppedSem_) {
            xSemaphoreGive(self->inboundStoppedSem_);
        }
        vTaskDelete(nullptr);
    }

    void VoiceAssistantManager::runInboundWorker() {
        ESP_LOGI(TAG, "Inbound worker started (stack %u words)", static_cast<unsigned>(INBOUND_TASK_STACK_WORDS));

        InboundMessage message{};
        while (true) {
            if (xQueueReceive(inboundQueue_, &message, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            if (message.type == InboundMessageType::SHUTDOWN) {
                break;
            }

            if (message.type == InboundMessageType::TEXT_JSON) {
                processTextMessage(static_cast<const char *>(message.payload));
            } else if (message.type == InboundMessageType::AUDIO_PCM) {
                processAudioResponse(static_cast<const uint8_t *>(message.payload), message.length);
            }

            freeInboundPayload(message);
        }

        ESP_LOGI(TAG, "Inbound worker stopped");
    }

    void VoiceAssistantManager::processAudioResponse(const uint8_t *data, size_t length) {
        if (!data || length == 0) {
            ESP_LOGW(TAG, "Received empty audio response");
            return;
        }

        esp_err_t ret = audioSourceManager.queueVoiceResponseChunk(data, length);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue voice response chunk: %s", esp_err_to_name(ret));
        }
    }

    void VoiceAssistantManager::onAudioInput(const uint8_t *data, size_t length) {
        if (!data || length == 0) {
            return;
        }

        static int64_t diag_window_start_us = 0;
        static uint32_t diag_chunks_received = 0;
        static uint32_t diag_chunks_sent = 0;
        static uint32_t diag_chunks_skipped_state = 0;
        static uint32_t diag_chunks_skipped_disconnected = 0;
        static uint32_t diag_chunks_skipped_spotify_protect = 0;
        static uint32_t diag_chunks_send_failed = 0;
        static uint64_t diag_bytes_sent = 0;
        static uint64_t diag_sum_send_us = 0;
        static uint32_t diag_max_send_us = 0;

        if (diagnostics_enabled) {
            if (diag_window_start_us == 0) {
                diag_window_start_us = esp_timer_get_time();
            }
            diag_chunks_received++;
        }

        const VoiceAssistantState current_state = state.load();

        if (current_state == VoiceAssistantState::LISTENING ||
            current_state == VoiceAssistantState::WAITING_FOR_USER) {
            const bool voice_response_draining =
                    audioSourceManager.getCurrentSource() == AudioSource::VOICE_RESPONSE &&
                    !audioSourceManager.isVoiceResponsePlaybackIdle();
            const bool defer_uplink_for_voice_response =
                    current_state == VoiceAssistantState::WAITING_FOR_USER && voice_response_draining;

            if (defer_uplink_for_voice_response) {
                if (websocketClient) {
                    websocketClient->setPlaybackMicSuspended(false);
                    websocketClient->setPlaybackStress(false);
                }
            } else if (websocketClient && websocketClient->isConnected()) {
                const AudioSource playback_source = audioSourceManager.getCurrentSource();
                const bool playback_active = playback_source == AudioSource::BLUETOOTH ||
                                             playback_source == AudioSource::SPOTIFY ||
                                             voice_response_draining;
                websocketClient->setPlaybackActive(playback_active);

                AudioSourceManager::SpotifyMicUplinkPolicy mic_policy =
                        AudioSourceManager::SpotifyMicUplinkPolicy::Allow;
                if (playback_source == AudioSource::SPOTIFY) {
                    mic_policy = audioSourceManager.getSpotifyMicUplinkPolicy();
                }

                const bool suspend_mic_for_spotify =
                        mic_policy == AudioSourceManager::SpotifyMicUplinkPolicy::Suspend;
                websocketClient->setPlaybackMicSuspended(suspend_mic_for_spotify);

                if (suspend_mic_for_spotify) {
                    websocketClient->setPlaybackStress(false);
                    if (diagnostics_enabled) {
                        diag_chunks_skipped_spotify_protect++;
                    }
                } else {
                    websocketClient->setPlaybackStress(
                            mic_policy == AudioSourceManager::SpotifyMicUplinkPolicy::Stress);

                    const int64_t before_send_us = esp_timer_get_time();
                    const esp_err_t ret = websocketClient->sendAudioChunk(data, length);
                    const uint32_t send_us =
                            static_cast<uint32_t>(esp_timer_get_time() - before_send_us);

                    if (ret == ESP_OK) {
                        if (diagnostics_enabled) {
                            diag_chunks_sent++;
                            diag_bytes_sent += length;
                            diag_sum_send_us += send_us;
                            if (send_us > diag_max_send_us) {
                                diag_max_send_us = send_us;
                            }
                            if (send_us > 100000) {
                                ESP_LOGW(TAG,
                                         "[UPSTREAM_DIAG] slow ws send %lu us, %u bytes, voice=%s audio=%s",
                                         static_cast<unsigned long>(send_us),
                                         static_cast<unsigned>(length),
                                         voiceStateName(current_state),
                                         audioSourceName(audioSourceManager.getCurrentSource()));
                            }
                        }
                    } else if (diagnostics_enabled) {
                        diag_chunks_send_failed++;
                        ESP_LOGE(TAG,
                                 "[UPSTREAM_DIAG] send failed: %s (%u bytes, voice=%s audio=%s)",
                                 esp_err_to_name(ret),
                                 static_cast<unsigned>(length),
                                 voiceStateName(current_state),
                                 audioSourceName(audioSourceManager.getCurrentSource()));
                    } else {
                        ESP_LOGE(TAG,
                                 "Upstream send failed: %s (%u bytes)",
                                 esp_err_to_name(ret),
                                 static_cast<unsigned>(length));
                    }
                }
            } else {
                if (diagnostics_enabled) {
                    diag_chunks_skipped_disconnected++;
                }
                if (websocketClient) {
                    websocketClient->setPlaybackMicSuspended(false);
                    websocketClient->setPlaybackStress(false);
                }
            }
        } else {
            if (diagnostics_enabled) {
                diag_chunks_skipped_state++;
            }
            if (websocketClient) {
                websocketClient->setPlaybackMicSuspended(false);
                websocketClient->setPlaybackStress(false);
            }
        }

        if (diagnostics_enabled) {
            const int64_t now_us = esp_timer_get_time();
            if ((now_us - diag_window_start_us) >= (UPSTREAM_DIAG_INTERVAL_MS * 1000LL)) {
                logUpstreamDiagnostics(now_us,
                                       diag_chunks_received,
                                       diag_chunks_sent,
                                       diag_chunks_skipped_state,
                                       diag_chunks_skipped_disconnected,
                                       diag_chunks_skipped_spotify_protect,
                                       diag_chunks_send_failed,
                                       diag_bytes_sent,
                                       diag_sum_send_us,
                                       diag_max_send_us);
                diag_window_start_us = now_us;
                diag_chunks_received = 0;
                diag_chunks_sent = 0;
                diag_chunks_skipped_state = 0;
                diag_chunks_skipped_disconnected = 0;
                diag_chunks_skipped_spotify_protect = 0;
                diag_chunks_send_failed = 0;
                diag_bytes_sent = 0;
                diag_sum_send_us = 0;
                diag_max_send_us = 0;
            }
        }
    }

    void VoiceAssistantManager::processTextMessage(const char *messageJson) {
        if (!messageJson) {
            ESP_LOGW(TAG, "Received null JSON message");
            return;
        }

        ESP_LOGD(TAG, "Processing text data: %s", messageJson);

        cJSON *json = cJSON_Parse(messageJson);
        if (!json) {
            ESP_LOGE(TAG, "Failed to parse JSON: %s", messageJson);
            return;
        }

        cJSON *typeItem = cJSON_GetObjectItem(json, "type");
        if (!typeItem || !cJSON_IsString(typeItem)) {
            ESP_LOGE(TAG, "Missing or invalid 'type' field in JSON");
            cJSON_Delete(json);
            return;
        }

        const char *type = cJSON_GetStringValue(typeItem);

        if (strcmp(type, "state_change") == 0) {
            cJSON *dataItem = cJSON_GetObjectItem(json, "data");
            if (!dataItem || !cJSON_IsString(dataItem)) {
                ESP_LOGE(TAG, "Missing or invalid 'data' field in state_change message");
                cJSON_Delete(json);
                return;
            }

            const char *stateStr = cJSON_GetStringValue(dataItem);
            VoiceAssistantState newState;

            if (strcmp(stateStr, "IDLE") == 0) {
                newState = VoiceAssistantState::IDLE;
            } else if (strcmp(stateStr, "LISTENING") == 0) {
                newState = VoiceAssistantState::LISTENING;
            } else if (strcmp(stateStr, "PROCESSING_INTENT") == 0) {
                newState = VoiceAssistantState::PROCESSING_INTENT;
            } else if (strcmp(stateStr, "STREAMING_RESPONSE") == 0) {
                newState = VoiceAssistantState::STREAMING_RESPONSE;
            } else if (strcmp(stateStr, "HANDLING_COMMAND") == 0) {
                newState = VoiceAssistantState::HANDLING_COMMAND;
            } else if (strcmp(stateStr, "WAITING_FOR_USER") == 0) {
                newState = VoiceAssistantState::WAITING_FOR_USER;
            } else {
                ESP_LOGW(TAG, "Unknown state received: %s", stateStr);
                cJSON_Delete(json);
                return;
            }

            ESP_LOGI(TAG, "Received state change: %s", stateStr);
            setVoiceAssistantState(newState);

        } else if (strcmp(type, "intent") == 0) {
            cJSON *intentItem = cJSON_GetObjectItem(json, "intent");
            cJSON *dataItem = cJSON_GetObjectItem(json, "data");

            if (!intentItem || !cJSON_IsString(intentItem) || !dataItem || !cJSON_IsString(dataItem)) {
                ESP_LOGE(TAG, "Missing or invalid 'intent' or 'data' field in intent message");
                cJSON_Delete(json);
                return;
            }

            const char *intent = cJSON_GetStringValue(intentItem);
            const char *data = cJSON_GetStringValue(dataItem);

            ESP_LOGI(TAG, "Received intent: %s, data: %s", intent, data);
            audioSourceManager.onMusicCommand(data);

        } else {
            ESP_LOGW(TAG, "Unknown message type received: %s", type);
        }

        cJSON_Delete(json);
    }

    void VoiceAssistantManager::setVoiceAssistantState(VoiceAssistantState new_state) {
        const VoiceAssistantState old_state = state.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "Voice assistant state changed from %d to %d", static_cast<int>(old_state),
                     static_cast<int>(new_state));
            if (new_state == VoiceAssistantState::LISTENING) {
                audioSourceManager.stopVoiceResponse();
            }
        }
    }

} // namespace btspeaker
