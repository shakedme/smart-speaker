#include "../include/voice_command_processor.hpp"
#include "../include/gemini_api_client.hpp"
#include "../include/audio_response_player.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
}

namespace btspeaker {

// Music command keywords for local processing
const char* VoiceCommandProcessor::MUSIC_COMMANDS[] = {
    "play", "pause", "stop", "resume", "next", "previous", 
    "volume up", "volume down", "skip", "start over"
};

const size_t VoiceCommandProcessor::MUSIC_COMMANDS_COUNT = 
    sizeof(MUSIC_COMMANDS) / sizeof(MUSIC_COMMANDS[0]);

VoiceCommandProcessor::VoiceCommandProcessor()
    : state_(State::IDLE)
    , state_mutex_(nullptr)
    , recording_buffer_pos_(0)
    , max_buffer_size_(0)
    , gemini_client_(nullptr)
    , response_player_(nullptr)
{
    ESP_LOGI(TAG, "VoiceCommandProcessor created");
}

VoiceCommandProcessor::~VoiceCommandProcessor() {
    shutdown();
}

esp_err_t VoiceCommandProcessor::init(GeminiApiClient* gemini_client, AudioResponsePlayer* response_player) {
    if (!gemini_client || !response_player) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    gemini_client_ = gemini_client;
    response_player_ = response_player;
    
    // Create state mutex
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize recording buffer
    max_buffer_size_ = (SAMPLE_RATE * BITS_PER_SAMPLE / 8) * (MAX_RECORDING_DURATION_MS / 1000);
    recording_buffer_ = std::make_unique<uint8_t[]>(max_buffer_size_);
    if (!recording_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Voice command processor initialized (buffer size: %zu bytes)", max_buffer_size_);
    return ESP_OK;
}

void VoiceCommandProcessor::shutdown() {
    ESP_LOGI(TAG, "Shutting down voice command processor");
    
    set_state(State::IDLE);
    
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    
    recording_buffer_.reset();
    recording_buffer_pos_ = 0;
    max_buffer_size_ = 0;
}

esp_err_t VoiceCommandProcessor::start_recording() {
    ESP_LOGI(TAG, "Starting voice command recording");
    
    if (state_.load() != State::IDLE) {
        ESP_LOGW(TAG, "Cannot start recording in current state: %d", static_cast<int>(state_.load()));
        return ESP_ERR_INVALID_STATE;
    }
    
    recording_buffer_pos_ = 0;
    set_state(State::RECORDING);
    
    ESP_LOGI(TAG, "Voice command recording started");
    return ESP_OK;
}

esp_err_t VoiceCommandProcessor::process_audio_data(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (state_.load() != State::RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if buffer has space
    if (recording_buffer_pos_ + length >= max_buffer_size_) {
        ESP_LOGW(TAG, "Recording buffer full, finishing recording");
        return finish_recording();
    }
    
    // Copy audio data to buffer
    memcpy(recording_buffer_.get() + recording_buffer_pos_, data, length);
    recording_buffer_pos_ += length;
    
    ESP_LOGD(TAG, "Processed %zu bytes, total: %zu/%zu", length, recording_buffer_pos_, max_buffer_size_);
    return ESP_OK;
}

esp_err_t VoiceCommandProcessor::finish_recording() {
    ESP_LOGI(TAG, "Finishing voice command recording (%zu bytes)", recording_buffer_pos_);
    
    if (state_.load() != State::RECORDING) {
        ESP_LOGW(TAG, "Not currently recording");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (recording_buffer_pos_ == 0) {
        ESP_LOGW(TAG, "No audio data recorded");
        set_state(State::IDLE);
        return ESP_ERR_INVALID_SIZE;
    }
    
    set_state(State::PROCESSING);
    
    // For now, we don't have speech-to-text to check for music commands
    // So we'll send everything to Gemini. In the future, we could add
    // local speech recognition for music commands.
    
    esp_err_t ret = send_to_gemini(recording_buffer_.get(), recording_buffer_pos_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send to Gemini: %s", esp_err_to_name(ret));
        set_state(State::ERROR);
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t VoiceCommandProcessor::cancel_recording() {
    ESP_LOGI(TAG, "Cancelling voice command recording");
    
    recording_buffer_pos_ = 0;
    set_state(State::IDLE);
    
    return ESP_OK;
}

void VoiceCommandProcessor::set_music_playback_callback(MusicPlaybackCallback callback) {
    music_callback_ = callback;
}

bool VoiceCommandProcessor::check_music_command(const char* text) {
    if (!text) {
        return false;
    }
    
    // Simple keyword matching for music commands
    for (size_t i = 0; i < MUSIC_COMMANDS_COUNT; i++) {
        if (strstr(text, MUSIC_COMMANDS[i]) != nullptr) {
            ESP_LOGI(TAG, "Music command detected: %s", MUSIC_COMMANDS[i]);
            if (music_callback_) {
                music_callback_(MUSIC_COMMANDS[i]);
            }
            return true;
        }
    }
    
    return false;
}

esp_err_t VoiceCommandProcessor::send_to_gemini(const uint8_t* audio_data, size_t length) {
    if (!gemini_client_ || !gemini_client_->is_ready()) {
        ESP_LOGE(TAG, "Gemini client not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Sending %zu bytes to Gemini API", length);
    
    // Create callback for handling Gemini response
    auto response_callback = [this](const uint8_t* audio_data, size_t length, bool is_final) {
        handle_gemini_response(audio_data, length, is_final);
    };
    
    esp_err_t ret = gemini_client_->send_audio_request(audio_data, length, response_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Gemini request: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

void VoiceCommandProcessor::handle_gemini_response(const uint8_t* audio_data, size_t length, bool is_final) {
    if (!response_player_) {
        ESP_LOGE(TAG, "Response player not available");
        return;
    }
    
    // Start playing response if not already playing
    if (state_.load() == State::PROCESSING) {
        set_state(State::PLAYING_RESPONSE);
    }
    
    // Queue audio chunk for playback
    esp_err_t ret = response_player_->queue_audio_chunk(audio_data, length, is_final);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue audio chunk: %s", esp_err_to_name(ret));
    }
    
    if (is_final) {
        // Response complete, return to idle
        ESP_LOGI(TAG, "Gemini response complete, returning to idle");
        set_state(State::IDLE);
        recording_buffer_pos_ = 0;
    }
}

void VoiceCommandProcessor::set_state(State new_state) {
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        State old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            ESP_LOGI(TAG, "State changed: %d -> %d", static_cast<int>(old_state), static_cast<int>(new_state));
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