#include "../include/voice_assistant_manager.hpp"
#include "../include/microphone_manager.hpp"
#include "../include/wake_word_detector.hpp"
#include "../include/voice_command_processor.hpp"
#include "../include/websocket_voice_client.hpp"
#include "../include/llm_pipeline.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

VoiceAssistantManager::VoiceAssistantManager()
    : state_(State::UNINITIALIZED)
    , state_mutex_(nullptr)
    , main_pipeline_manager_(nullptr)
{
    ESP_LOGI(TAG, "VoiceAssistantManager created");
}

VoiceAssistantManager::~VoiceAssistantManager() {
    shutdown();
}

esp_err_t VoiceAssistantManager::init(BluetoothPipeline* main_pipeline_manager) {
    if (!main_pipeline_manager) {
        ESP_LOGW(TAG, "Main pipeline manager is null");
//        return ESP_ERR_INVALID_ARG;
    }
    
    main_pipeline_manager_ = main_pipeline_manager;
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize WebSocket voice client
    WebSocketVoiceClient::Config websocket_config;
    websocket_config.server_url = CONFIG_WEBSOCKET_SERVER_URL;
    websocket_config.keepalive_interval_s = CONFIG_WS_KEEPALIVE_INTERVAL_S;
    websocket_config.ping_interval_s = CONFIG_WS_PING_INTERVAL_S;
    websocket_config.chunk_size = CONFIG_WS_CHUNK_SIZE;
    websocket_config.reconnect_timeout_ms = CONFIG_WS_RECONNECT_TIMEOUT_MS;
    websocket_config.max_reconnect_attempts = CONFIG_WS_MAX_RECONNECT_ATTEMPTS;
    
    websocket_client_ = std::make_unique<WebSocketVoiceClient>(websocket_config);
    esp_err_t ret = websocket_client_->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Attempt to connect to WebSocket server
    ret = websocket_client_->connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to connect to WebSocket server: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Will operate in offline mode until connection is established");
    }
    
    // Note: AudioSourceManager handles voice response playback now
    // We don't need a separate response player in the voice assistant manager
    
    // Initialize voice command processor
    command_processor_ = std::make_unique<VoiceCommandProcessor>();
    ret = command_processor_->init(websocket_client_.get());  // AudioSourceManager handles audio
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize command processor: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize wake word detector
    wake_word_detector_ = std::make_unique<WakeWordDetector>();
    ret = wake_word_detector_->init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wake word detector initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without wake word detection");
    }
    
    // Initialize microphone manager
    microphone_manager_ = std::make_unique<MicrophoneManager>();
    ret = microphone_manager_->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize microphone manager: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set up callbacks
    microphone_manager_->set_audio_callback(
        [this](const uint8_t* data, size_t length) {
            on_audio_data(data, length);
        }
    );
    
    wake_word_detector_->set_wake_word_callback(
        [this]() {
            on_wake_word_detected();
        }
    );
    
    set_state(State::LISTENING);
    ESP_LOGI(TAG, "Voice assistant manager initialized successfully");
    return ESP_OK;
}

esp_err_t VoiceAssistantManager::start() {
    if (state_.load() == State::UNINITIALIZED) {
        ESP_LOGE(TAG, "Voice assistant not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Start microphone recording
    esp_err_t ret = microphone_manager_->start_recording();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start microphone recording: %s", esp_err_to_name(ret));
        return ret;
    }
    
    set_state(State::LISTENING);
    ESP_LOGI(TAG, "Voice assistant started");
    return ESP_OK;
}

esp_err_t VoiceAssistantManager::stop() {
    ESP_LOGI(TAG, "Stopping voice assistant");
    
    // Stop microphone recording
    if (microphone_manager_) {
        microphone_manager_->stop_recording();
    }
    
    // Cancel any ongoing command processing
    if (command_processor_) {
        command_processor_->cancel_recording();
    }
    
    set_state(State::LISTENING);
    return ESP_OK;
}

void VoiceAssistantManager::shutdown() {
    ESP_LOGI(TAG, "Shutting down voice assistant");
    
    set_state(State::UNINITIALIZED);
    
    // Shutdown all components
    if (microphone_manager_) {
        microphone_manager_->shutdown();
        microphone_manager_.reset();
    }
    
    if (wake_word_detector_) {
        wake_word_detector_->shutdown();
        wake_word_detector_.reset();
    }
    
    if (command_processor_) {
        command_processor_->shutdown();
        command_processor_.reset();
    }
    
    // AudioSourceManager handles voice response cleanup
    
    if (websocket_client_) {
        websocket_client_->shutdown();
        websocket_client_.reset();
    }
    
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
}

void VoiceAssistantManager::set_music_playback_callback(MusicPlaybackCallback callback) {
    music_callback_ = callback;
    if (command_processor_) {
        command_processor_->set_music_playback_callback(callback);
    }
}

esp_err_t VoiceAssistantManager::trigger_voice_command() {
    ESP_LOGI(TAG, "Manually triggering voice command");
    
    State current_state = state_.load();
    if (current_state != State::LISTENING) {
        ESP_LOGW(TAG, "Cannot trigger voice command in current state: %d", static_cast<int>(current_state));
        return ESP_ERR_INVALID_STATE;
    }
    
    // Simulate wake word detection
    on_wake_word_detected();
    return ESP_OK;
}

void VoiceAssistantManager::on_audio_data(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return;
    }
    
    // In the new architecture, the WebSocket client manages the streaming state.
    // We just continuously send audio data when the voice assistant is in the RECORDING_COMMAND state.
    if (state_.load() == State::RECORDING_COMMAND) {
        // Stream audio to backend via WebSocketVoiceClient
        if (websocket_client_ && websocket_client_->is_connected()) {
            esp_err_t ret = websocket_client_->send_audio_chunk(data, length);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to stream audio to backend: %s", esp_err_to_name(ret));
                // The websocket client will handle disconnection and attempt reconnection.
                // We can transition to an error state or just log here.
                set_state(State::ERROR); 
            }
        }
    }
    // Wake word detection is assumed to be handled before transitioning to RECORDING_COMMAND.
    // For simplicity, we are not showing the wake word logic here but it would gate the transition.
}

void VoiceAssistantManager::on_wake_word_detected() {
    ESP_LOGI(TAG, "Wake word detected, starting command recording");
    
    if(state_.load() == State::RECORDING_COMMAND) {
        ESP_LOGW(TAG, "Already recording, ignoring wake word.");
        return;
    }

    set_state(State::RECORDING_COMMAND);

    // Ensure the microphone is running.
    if (microphone_manager_) {
        microphone_manager_->start_recording();
    }

    // The WebSocket client will start streaming automatically when it connects.
    // If it's already connected, it will start streaming when its state is ready.
    // If not connected, we ensure a connection attempt is made.
    if (websocket_client_ && !websocket_client_->is_connected()) {
        ESP_LOGI(TAG, "WebSocket not connected, attempting to connect...");
        websocket_client_->connect();
    } else if (websocket_client_) {
        // If already connected, ensure recording session starts.
        // The websocket client now handles this automatically upon connection,
        // but a manual start is good for robustness.
        websocket_client_->start_recording_session();
    }
}

void VoiceAssistantManager::set_state(State new_state) {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
            
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
                    set_state(State::LISTENING);
                    break;
                    
                default:
                    break;
            }
        }
        xSemaphoreGive(state_mutex_);
    } else {
        // Fallback without mutex protection
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGW(TAG, "State changed without mutex: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
        }
    }
}

} // namespace btspeaker 