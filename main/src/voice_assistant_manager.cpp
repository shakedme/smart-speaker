#include "../include/voice_assistant_manager.hpp"
#include "../include/microphone_manager.hpp"
#include "../include/websocket_voice_client.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "cJSON.h"
}

namespace btspeaker {

    VoiceAssistantManager::VoiceAssistantManager(AudioSourceManager &audioSourceManager)
            :
            state(VoiceAssistantState::IDLE),
            stateMutex(nullptr),
            audioSourceManager(audioSourceManager) {
        // Create state mutex
        stateMutex = xSemaphoreCreateMutex();
        if (!stateMutex) {
            ESP_LOGE(TAG, "Failed to create state mutex");
            throw std::runtime_error("Failed to create state mutex");
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
        shutdown();
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

    esp_err_t VoiceAssistantManager::stop() {
        ESP_LOGI(TAG, "Stopping voice assistant");

        // Stop microphone recording
        if (microphoneManager) {
            microphoneManager->stopRecording();
        }

        if (websocketClient) {
            websocketClient->cancelOperations();
        }

        setVoiceAssistantState(VoiceAssistantState::IDLE);
        return ESP_OK;
    }

    void VoiceAssistantManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down voice assistant");

        setVoiceAssistantState(VoiceAssistantState::IDLE);

        if (microphoneManager) {
            microphoneManager->shutdown();
            microphoneManager.reset();
        }

        if (websocketClient) {
            websocketClient->shutdown();
            websocketClient.reset();
        }

        if (stateMutex) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
        }
    }

    void VoiceAssistantManager::onAudioResponse(const uint8_t *data, size_t length) {
        if (!data || length == 0) {
            ESP_LOGW(TAG, "Received empty audio response");
            return;
        }
        audioSourceManager.queueVoiceResponseChunk(data, length);
    }

    void VoiceAssistantManager::onAudioInput(const uint8_t *data, size_t length) {
        if (!data || length == 0) {
            return;
        }

        VoiceAssistantState current_state = state.load();

        if (current_state == VoiceAssistantState::LISTENING || current_state == VoiceAssistantState::WAITING_FOR_USER) {
            if (websocketClient->isConnected()) {
                esp_err_t ret = websocketClient->sendAudioChunk(data, length);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to stream audio to backend: %s", esp_err_to_name(ret));
                }
            }
        }
    }

    void VoiceAssistantManager::onTextData(const char *messageJson) {
        if (!messageJson) {
            ESP_LOGW(TAG, "Received null JSON message");
            return;
        }

        ESP_LOGD(TAG, "Received text data: %s", messageJson);

        cJSON *json = cJSON_Parse(messageJson);
        if (!json) {
            ESP_LOGE(TAG, "Failed to parse JSON: %s", messageJson);
            return;
        }

        // Get the message type
        cJSON *typeItem = cJSON_GetObjectItem(json, "type");
        if (!typeItem || !cJSON_IsString(typeItem)) {
            ESP_LOGE(TAG, "Missing or invalid 'type' field in JSON");
            cJSON_Delete(json);
            return;
        }

        const char *type = cJSON_GetStringValue(typeItem);
        
        if (strcmp(type, "state_change") == 0) {
            // Handle state change message: {"type": "state_change", "data": "LISTENING"}
            cJSON *dataItem = cJSON_GetObjectItem(json, "data");
            if (!dataItem || !cJSON_IsString(dataItem)) {
                ESP_LOGE(TAG, "Missing or invalid 'data' field in state_change message");
                cJSON_Delete(json);
                return;
            }

            const char *stateStr = cJSON_GetStringValue(dataItem);
            VoiceAssistantState newState;

            // Map string states to enum values
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
            // Handle intent message: {"type": "intent", "intent": "spotify", "data": "<uri>"}
            // or {"type": "intent", "intent": "playback_control", "data": "pause"}
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

            // Pass the data to audioSourceManager for music command processing
            audioSourceManager.onMusicCommand(data);

        } else {
            ESP_LOGW(TAG, "Unknown message type received: %s", type);
        }

        cJSON_Delete(json);
    }

    void VoiceAssistantManager::setVoiceAssistantState(VoiceAssistantState new_state) {
        if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            VoiceAssistantState old_state = state.exchange(new_state);
            switch (new_state) {
                case VoiceAssistantState::LISTENING:
                    audioSourceManager.stopVoiceResponse();
                    break;
                default:
                    break;
            }
            xSemaphoreGive(stateMutex);
        } else {
            // Fallback without mutex protection
            VoiceAssistantState old_state = state.exchange(new_state);
            if (old_state != new_state) {

            }
        }
    }


} // namespace btspeaker 