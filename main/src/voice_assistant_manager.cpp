#include "../include/voice_assistant_manager.hpp"
#include "../include/microphone_manager.hpp"
#include "../include/wake_word_detector.hpp"
#include "../include/voice_command_processor.hpp"
#include "../include/gemini_api_client.hpp"
#include "../include/audio_response_player.hpp"
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

esp_err_t VoiceAssistantManager::init(AudioPipelineManager* main_pipeline_manager) {
    if (!main_pipeline_manager) {
        ESP_LOGE(TAG, "Main pipeline manager is null");
        return ESP_ERR_INVALID_ARG;
    }
    
    main_pipeline_manager_ = main_pipeline_manager;
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize Gemini API client
    GeminiApiClient::Config gemini_config;
    gemini_config.api_key = CONFIG_GEMINI_API_KEY;
    gemini_client_ = std::make_unique<GeminiApiClient>(gemini_config);
    esp_err_t ret = gemini_client_->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Gemini client: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize audio response player
    response_player_ = std::make_unique<AudioResponsePlayer>();
    ret = response_player_->init(main_pipeline_manager_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize response player: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize voice command processor
    command_processor_ = std::make_unique<VoiceCommandProcessor>();
    ret = command_processor_->init(gemini_client_.get(), response_player_.get());
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
    
    if (response_player_) {
        response_player_->shutdown();
        response_player_.reset();
    }
    
    if (gemini_client_) {
        gemini_client_->shutdown();
        gemini_client_.reset();
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
    
    State current_state = state_.load();
    
    switch (current_state) {
        case State::LISTENING:
            // Process audio for wake word detection
            if (wake_word_detector_ && wake_word_detector_->is_enabled()) {
                const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                size_t sample_count = length / sizeof(int16_t);
                
                bool wake_word_detected = wake_word_detector_->is_wake_word_in_audio(samples, sample_count);
                if (wake_word_detected) {
                    ESP_LOGI(TAG, "Wake word detected via audio processing!");
                    on_wake_word_detected();
                }
            }
            break;
            
        case State::RECORDING_COMMAND:
            // Forward audio to command processor
            if (command_processor_ && command_processor_->is_recording()) {
                esp_err_t ret = command_processor_->process_audio_data(data, length);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to process audio data: %s", esp_err_to_name(ret));
                }
            }
            break;
            
        case State::WAKE_WORD_DETECTED:
        case State::PROCESSING_GEMINI:
        case State::PLAYING_RESPONSE:
        case State::ERROR:
        case State::UNINITIALIZED:
        default:
            // Ignore audio in other states
            break;
    }
}

void VoiceAssistantManager::on_wake_word_detected() {
    ESP_LOGI(TAG, "Wake word detected! Starting command recording");
    
    if (state_.load() != State::LISTENING) {
        ESP_LOGW(TAG, "Wake word detected but not in listening state");
        return;
    }
    
    set_state(State::WAKE_WORD_DETECTED);
    
    // Start recording voice command
    if (command_processor_) {
        esp_err_t ret = command_processor_->start_recording();
        if (ret == ESP_OK) {
            set_state(State::RECORDING_COMMAND);
        } else {
            ESP_LOGE(TAG, "Failed to start command recording: %s", esp_err_to_name(ret));
            set_state(State::ERROR);
        }
    }
}

void VoiceAssistantManager::set_state(State new_state) {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
            
            // Handle state transitions
            switch (new_state) {
                case State::LISTENING:
                    // Back to listening - ready for wake word
                    break;
                    
                case State::RECORDING_COMMAND:
                    // Started recording - set timeout to auto-finish
                    // TODO: Add timeout mechanism
                    break;
                    
                case State::PROCESSING_GEMINI:
                    // Command sent to Gemini - wait for response
                    break;
                    
                case State::PLAYING_RESPONSE:
                    // Playing Gemini response
                    break;
                    
                case State::ERROR:
                    // Error state - return to listening after delay
                    ESP_LOGW(TAG, "Entering error state, will return to listening");
                    // Don't recursively call set_state - just schedule return to listening
                    old_state = state_.exchange(State::LISTENING);
                    ESP_LOGI(TAG, "Recovered from error state: %d -> %d", static_cast<int>(old_state), static_cast<int>(State::LISTENING));
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