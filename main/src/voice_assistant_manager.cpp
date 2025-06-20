#include "../include/voice_assistant_manager.hpp"
#include "../include/microphone_manager.hpp"
#include "../include/wake_word_detector.hpp"
#include "../include/voice_command_processor.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/llm_pipeline.hpp"
#include "../include/voice_config.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

    VoiceAssistantManager::VoiceAssistantManager(AudioSourceManager& audioSourceManager)
            :
            state(State::UNINITIALIZED),
            stateMutex(nullptr),
            audioSourceManager(audioSourceManager){
        // Create state mutex
        stateMutex = xSemaphoreCreateMutex();
        if (!stateMutex) {
            ESP_LOGE(TAG, "Failed to create state mutex");
            throw std::runtime_error("Failed to create state mutex");
        }

        // Initialize WebSocket voice client
        WebSocketVoiceClient::Config websocket_config;
        websocket_config.serverUrl = CONFIG_WEBSOCKET_SERVER_URL;
        websocket_config.keepaliveIntervalS = CONFIG_WS_KEEPALIVE_INTERVAL_S;
        websocket_config.pingIntervalS = CONFIG_WS_PING_INTERVAL_S;
        websocket_config.chunkSize = CONFIG_WS_CHUNK_SIZE;
        websocket_config.reconnectTimeoutMs = CONFIG_WS_RECONNECT_TIMEOUT_MS;
        websocket_config.maxReconnectAttempts = CONFIG_WS_MAX_RECONNECT_ATTEMPTS;

        websocketClient = std::make_unique<WebSocketVoiceClient>(websocket_config);
        esp_err_t ret = websocketClient->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WebSocket client: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize WebSocket client");
        }

        // Attempt to connect to WebSocket server
        ret = websocketClient->connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to connect to WebSocket server: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to connect to WebSocket server");
        }

        // Initialize voice command processor
        voiceCommandProcessor = std::make_unique<VoiceCommandProcessor>(audioSourceManager);
        ret = voiceCommandProcessor->init(websocketClient.get());  // AudioSourceManager handles audio
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize command processor: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize command processor");
        }

        // Initialize wake word detector
        wakeWordDetector = std::make_unique<WakeWordDetector>();
        ret = wakeWordDetector->init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Wake word detector initialization failed: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize wake word detector");
        }

        // Initialize microphone manager
        microphoneManager = std::make_unique<MicrophoneManager>();
        ret = microphoneManager->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize microphone manager: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize microphone manager");
        }

        microphoneManager->setAudioCallback(
                [this](const uint8_t *data, size_t length) {
                    onAudioData(data, length);
                }
        );

        wakeWordDetector->setWakeWordCallback(
                [this]() {
                    onWakeWordDetected();
                }
        );

        setState(State::LISTENING);
        ESP_LOGI(TAG, "VoiceAssistantManager initialized successfully");
    }

    VoiceAssistantManager::~VoiceAssistantManager() {
        shutdown();
    }

    esp_err_t VoiceAssistantManager::start() {
        if (state.load() == State::UNINITIALIZED) {
            ESP_LOGE(TAG, "Voice assistant not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        // Start microphone recording
        esp_err_t ret = microphoneManager->startRecording();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start microphone recording: %s", esp_err_to_name(ret));
            return ret;
        }

        setState(State::LISTENING);
        ESP_LOGI(TAG, "Voice assistant started");
        return ESP_OK;
    }

    esp_err_t VoiceAssistantManager::stop() {
        ESP_LOGI(TAG, "Stopping voice assistant");

        // Stop microphone recording
        if (microphoneManager) {
            microphoneManager->stopRecording();
        }

        // Cancel any ongoing command processing
        if (voiceCommandProcessor) {
            voiceCommandProcessor->cancelRecording();
        }

        setState(State::LISTENING);
        return ESP_OK;
    }

    void VoiceAssistantManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down voice assistant");

        setState(State::UNINITIALIZED);

        // Shutdown all components
        if (microphoneManager) {
            microphoneManager->shutdown();
            microphoneManager.reset();
        }

        if (wakeWordDetector) {
            wakeWordDetector->shutdown();
            wakeWordDetector.reset();
        }

        if (voiceCommandProcessor) {
            voiceCommandProcessor->shutdown();
            voiceCommandProcessor.reset();
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

    void VoiceAssistantManager::setMusicPlaybackCallback(const MusicPlaybackCallback &callback) {
        musicCallback = callback;
        if (voiceCommandProcessor) {
            voiceCommandProcessor->setMusicPlaybackCallback(callback);
        }
    }

    void VoiceAssistantManager::onAudioData(const uint8_t *data, size_t length) {
        if (!data || length == 0) {
            return;
        }

        State current_state = state.load();

        // We just continuously send audio data when the voice assistant is in the RECORDING_COMMAND state.
        if (current_state == State::RECORDING_COMMAND) {
            // Stream audio to backend via WebSocketVoiceClient
            if (websocketClient && websocketClient->isConnected()) {
                esp_err_t ret = websocketClient->sendAudioChunk(data, length);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to stream audio to backend: %s", esp_err_to_name(ret));
                    setState(State::ERROR);
                }
            }
        } else if (current_state == State::LISTENING) {
            if (wakeWordDetector && wakeWordDetector->isEnabled()) {
                esp_err_t ret = wakeWordDetector->feedAudioData(data, length);
                if (ret != ESP_OK && ret != ESP_ERR_NO_MEM) {
                    // ESP_ERR_NO_MEM is expected when ring buffer is full (just drop data)
                    ESP_LOGD(TAG, "Failed to feed audio to wake word detector: %s", esp_err_to_name(ret));
                }
            }
        }
    }

    void VoiceAssistantManager::onWakeWordDetected() {
        ESP_LOGI(TAG, "Wake word detected, starting command recording");

        if (state.load() == State::RECORDING_COMMAND) {
            ESP_LOGW(TAG, "Already recording, ignoring wake word.");
            return;
        }

        setState(State::RECORDING_COMMAND);

        // The WebSocket client will start streaming automatically when it connects.
        if (websocketClient && !websocketClient->isConnected()) {
            ESP_LOGI(TAG, "WebSocket not connected, attempting to connect...");
            websocketClient->connect();
        }
    }

    void VoiceAssistantManager::setState(State new_state) {
        if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            State old_state = state.exchange(new_state);
            if (old_state != new_state) {
                ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));

                // Handle wake word detector state based on new state
                if (wakeWordDetector) {
                    switch (new_state) {
                        case State::LISTENING:
                            // Enable wake word detection when listening
                            if (!wakeWordDetector->isEnabled()) {
                                wakeWordDetector->setEnabled(true);
                                ESP_LOGI(TAG, "Wake word detection enabled for listening state");
                            }
                            break;

                        case State::RECORDING_COMMAND:
                        case State::PROCESSING_COMMAND:
                        case State::PLAYING_RESPONSE:
                            // Disable wake word detection during other states to save CPU
                            if (wakeWordDetector->isEnabled()) {
                                wakeWordDetector->setEnabled(false);
                                ESP_LOGI(TAG, "Wake word detection disabled during %s state",
                                         new_state == State::RECORDING_COMMAND ? "recording" :
                                         new_state == State::PROCESSING_COMMAND ? "processing" : "playback");
                            }
                            break;

                        default:
                            break;
                    }
                }

                // Handle state-specific actions
                switch (new_state) {
                    case State::PROCESSING_COMMAND:
                        ESP_LOGI(TAG, "Processing voice command via WebSocket");
                        break;

                    case State::PLAYING_RESPONSE:
                        ESP_LOGI(TAG, "Playing audio response");
                        break;

                    case State::ERROR:
                        ESP_LOGE(TAG, "Voice assistant entered error state");
                        // Attempt to recover by returning to listening state
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        setState(State::LISTENING);
                        break;

                    default:
                        break;
                }
            }
            xSemaphoreGive(stateMutex);
        } else {
            // Fallback without mutex protection
            State old_state = state.exchange(new_state);
            if (old_state != new_state) {
                ESP_LOGW(TAG, "State changed without mutex: %d -> %d", static_cast<int>(old_state),
                         static_cast<int>(new_state));
            }
        }
    }


} // namespace btspeaker 