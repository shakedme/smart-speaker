#include "../include/voice_command_processor.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/audio_source_manager.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#include <utility>
#include "cJSON.h"
}

namespace btspeaker {

    VoiceCommandProcessor::VoiceCommandProcessor()
            : state(State::IDLE), stateMutex(nullptr), websocketClient(nullptr) {
        ESP_LOGI(TAG, "VoiceCommandProcessor created");
    }

    VoiceCommandProcessor::~VoiceCommandProcessor() {
        shutdown();
    }

    esp_err_t VoiceCommandProcessor::init(WebSocketVoiceClient *websocket_client) {
        if (!websocket_client) {
            ESP_LOGE(TAG, "Invalid parameters");
            return ESP_ERR_INVALID_ARG;
        }

        websocketClient = websocket_client;

        // Create state mutex
        stateMutex = xSemaphoreCreateMutex();
        if (!stateMutex) {
            ESP_LOGE(TAG, "Failed to create state mutex");
            return ESP_ERR_NO_MEM;
        }

        // Set up WebSocket callbacks
        auto audio_callback = [this](const uint8_t *audio_data, size_t length, bool is_final, const char *intent_json) {
            handleWebsocketResponse(audio_data, length, is_final, intent_json);
        };
        websocketClient->setAudioResponseCallback(audio_callback);

        auto state_callback = [this](WebSocketVoiceClient::ConnectionState new_state) {
            handleConnectionStateChange(static_cast<int>(new_state));
        };
        websocketClient->setStateChangeCallback(state_callback);

        ESP_LOGI(TAG, "Voice command processor initialized");
        return ESP_OK;
    }

    void VoiceCommandProcessor::shutdown() {
        ESP_LOGI(TAG, "Shutting down voice command processor");

        if (stateMutex) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
        }

        state.store(State::IDLE);
    }

    esp_err_t VoiceCommandProcessor::cancelRecording() {
        ESP_LOGI(TAG, "Cancelling voice command processing");

        if (websocketClient && websocketClient->isConnected()) {
            websocketClient->cancelOperations();
        }

        setState(State::IDLE);
        return ESP_OK;
    }

    void VoiceCommandProcessor::setMusicPlaybackCallback(MusicPlaybackCallback callback) {
        musicCallback = std::move(callback);
    }


    void VoiceCommandProcessor::handleWebsocketResponse(const uint8_t *audioData, size_t length, bool isFinal,
                                                        const char *messageJson) {
        // Handle binary audio data (voice response streaming)
        if (audioData && length > 0) {
            ESP_LOGI(TAG, "Received TTS audio from backend (%zu bytes)", length);
            setState(State::PLAYING_RESPONSE);
            // Queue audio chunk for streaming playback via AudioSourceManager
            auto &audio_source_mgr = AudioSourceManager::getInstance();
            audio_source_mgr.queueVoiceResponseChunk(audioData, length, false);
            return;
        }

        // Handle text messages (state changes and intents)
        if (messageJson && strlen(messageJson) > 0) {
            cJSON *root = cJSON_Parse(messageJson);
            if (!root) {
                ESP_LOGE(TAG, "Failed to parse message JSON");
                return;
            }

            // Check for state change messages
            cJSON *type_item = cJSON_GetObjectItem(root, "type");
            if (type_item && cJSON_IsString(type_item)) {
                std::string msg_type = type_item->valuestring;

                if (msg_type == "state_change") {
                    cJSON *state_item = cJSON_GetObjectItem(root, "state");
                    if (state_item && cJSON_IsString(state_item)) {
                        handleBackendStateChange(state_item->valuestring);
                    }
                    cJSON_Delete(root);
                    return;
                }
            }

            // Handle intent messages (is_final indicates this is an intent, not a state change)
            if (isFinal) {
                ESP_LOGI(TAG, "Received intent JSON from backend: %s", messageJson);
                setState(State::PROCESSING);

                cJSON *intent_item = cJSON_GetObjectItem(root, "intent");
                if (intent_item && cJSON_IsString(intent_item)) {
                    std::string intent_type = intent_item->valuestring;
                    cJSON *data_item = cJSON_GetObjectItem(root, "data");
                    std::string data_value = (data_item && cJSON_IsString(data_item)) ? data_item->valuestring : "";

                    if (intent_type == "spotify") {
                        ESP_LOGI(TAG, "Handling Spotify intent with data: %s", data_value.c_str());
                        if (musicCallback && !data_value.empty()) {
                            musicCallback(data_value.c_str());
                        }
                        setState(State::IDLE);
                    } else if (intent_type == "playback_control") {
                        ESP_LOGI(TAG, "Handling playback control intent: %s", data_value.c_str());
                        if (musicCallback && !data_value.empty()) {
                            musicCallback(data_value.c_str());
                        }
                        setState(State::IDLE);
                    } else if (intent_type == "information") {
                        ESP_LOGI(TAG, "Information intent acknowledged. TTS audio will stream.");
                        // State will be managed by backend state changes
                    } else {
                        ESP_LOGW(TAG, "Unknown intent type: %s", intent_type.c_str());
                        setState(State::IDLE);
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    void VoiceCommandProcessor::handleBackendStateChange(const std::string &backendState) {
        ESP_LOGI(TAG, "Backend state changed to: %s", backendState.c_str());

        if (backendState == "LISTENING") {
            ESP_LOGI(TAG, "Backend is listening for commands");
            setState(State::IDLE);
            // Stop any voice response playback
            auto &audio_source_mgr = AudioSourceManager::getInstance();
            audio_source_mgr.stopVoiceResponse();
        } else if (backendState == "PROCESSING_INTENT") {
            ESP_LOGI(TAG, "Backend is processing intent");
            setState(State::PROCESSING);
        } else if (backendState == "STREAMING_RESPONSE") {
            ESP_LOGI(TAG, "Backend is streaming voice response");
            setState(State::PLAYING_RESPONSE);
        } else if (backendState == "WAITING_FOR_USER") {
            ESP_LOGI(TAG, "Backend is waiting for user response (conversation mode)");
            // Stop voice response playback but keep session active
            auto &audio_source_mgr = AudioSourceManager::getInstance();
            audio_source_mgr.queueVoiceResponseChunk(nullptr, 0, true);
            setState(State::IDLE);
        } else if (backendState == "HANDLING_COMMAND") {
            ESP_LOGI(TAG, "Backend is handling command");
            setState(State::PROCESSING);
        } else {
            ESP_LOGW(TAG, "Unknown backend state: %s", backendState.c_str());
        }
    }

    void VoiceCommandProcessor::handleConnectionStateChange(int connectionState) {
        auto ws_state = static_cast<WebSocketVoiceClient::ConnectionState>(connectionState);

        switch (ws_state) {
            case WebSocketVoiceClient::ConnectionState::CONNECTED:
                ESP_LOGI(TAG, "WebSocket server connected - online mode available");
                setState(State::IDLE);
                break;

            case WebSocketVoiceClient::ConnectionState::CONNECTING:
                ESP_LOGI(TAG, "WebSocket connecting...");
                break;

            case WebSocketVoiceClient::ConnectionState::DISCONNECTED: {
                ESP_LOGW(TAG, "WebSocket server disconnected - going idle");
                // Finalize any audio that was playing
                auto &audio_source_mgr = AudioSourceManager::getInstance();
                audio_source_mgr.stopVoiceResponse();
                setState(State::IDLE);
                break;
            }

            case WebSocketVoiceClient::ConnectionState::ERROR: {
                ESP_LOGW(TAG, "WebSocket server unavailable - using offline mode");
                auto &audio_source_mgr = AudioSourceManager::getInstance();
                audio_source_mgr.stopVoiceResponse();
                setState(State::IDLE);
                break;
            }
        }
    }

    void VoiceCommandProcessor::setState(State newState) {
        if (!stateMutex) {
            state.store(newState);
            return;
        }

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            State old_state = state.load();
            state.store(newState);

            if (old_state != newState) {
                ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(newState));
            }

            xSemaphoreGive(stateMutex);
        } else {
            ESP_LOGW(TAG, "Failed to acquire state mutex, setting state directly");
            state.store(newState);
        }
    }

} // namespace btspeaker 