#include "../include/wake_word_detector.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_wn_models.h"
#include "model_path.h"
}

namespace btspeaker {

WakeWordDetector::WakeWordDetector()
    : enabled_(false)
    , threshold_(CONFIG_WAKE_WORD_THRESHOLD)
    , afe_handle_(nullptr)
    , afe_data_(nullptr)
    , wake_word_models_(nullptr)
{
    ESP_LOGI(TAG, "WakeWordDetector created");
}

WakeWordDetector::~WakeWordDetector() {
    shutdown();
}

esp_err_t WakeWordDetector::init() {
    ESP_LOGI(TAG, "Initializing wake word detector");
    
    esp_err_t ret = setup_esp_sr();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR setup failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Wake word detection will be disabled");
        enabled_ = false;
        return ESP_OK; // Don't fail initialization
    }
    
    enabled_ = CONFIG_WAKE_WORD_ENABLED;
    ESP_LOGI(TAG, "Wake word detector initialized (enabled: %s)", enabled_ ? "yes" : "no");
    return ESP_OK;
}

void WakeWordDetector::shutdown() {
    ESP_LOGI(TAG, "Shutting down wake word detector");
    
    enabled_ = false;
    cleanup_esp_sr();
}

bool WakeWordDetector::is_wake_word_in_audio(const int16_t* audio_data, size_t sample_count) {
    if (!enabled_ || !audio_data || sample_count == 0) {
        return false;
    }
    
    if (!afe_handle_ || !afe_data_) {
        return false;
    }
    
    // Feed audio data to ESP-SR AFE
    esp_afe_sr_iface_t* iface = (esp_afe_sr_iface_t*)afe_handle_;
    int feed_result = iface->feed((esp_afe_sr_data_t*)afe_data_, audio_data);
    
    if (feed_result <= 0) {
        ESP_LOGD(TAG, "AFE feed failed: %d", feed_result);
        return false;
    }
    
    // Fetch result from AFE
    afe_fetch_result_t* fetch_result = iface->fetch((esp_afe_sr_data_t*)afe_data_);
    if (!fetch_result) {
        return false;
    }
    
    // Check for wake word detection
    if (fetch_result->wakeup_state == WAKENET_DETECTED) {
        ESP_LOGI(TAG, "Wake word detected! Index: %d, Channel: %d", 
                 fetch_result->wake_word_index, fetch_result->trigger_channel_id);
        
        if (wake_word_callback_) {
            wake_word_callback_();
        }
        return true;
    }
    
    return false;
}

void WakeWordDetector::set_wake_word_callback(WakeWordCallback callback) {
    wake_word_callback_ = callback;
}

esp_err_t WakeWordDetector::setup_esp_sr() {
    ESP_LOGI(TAG, "Setting up ESP-SR with memory optimizations");
    
    // For compiled-in models (enabled via menuconfig), use NULL instead of partition name
    srmodel_list_t *models = esp_srmodel_init(NULL);
    if (!models) {
        ESP_LOGW(TAG, "No compiled-in SR models found. Please enable wake word models in menuconfig:");
        ESP_LOGW(TAG, "Component config -> ESP Speech Recognition -> Wake Word Engine -> Enable");
        ESP_LOGW(TAG, "Component config -> ESP Speech Recognition -> Wake Word Engine -> Select wn9_hiesp");
        return ESP_FAIL;
    }
    
    // Create AFE configuration with memory-optimized settings
    // Input format: "M" means single microphone channel
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "Failed to create AFE config");
        return ESP_FAIL;
    }
    
    // Customize configuration for our hardware
    afe_config->pcm_config.sample_rate = CONFIG_MIC_SAMPLE_RATE;
    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;  // No reference channel for AEC yet
    
    // Wake word configuration
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = const_cast<char*>("wn9_hiesp");  // Use "Hi ESP" wake word
    afe_config->wakenet_mode = DET_MODE_95;  // More conservative detection to reduce processing
    
    // MEMORY OPTIMIZATION: Disable all non-essential audio processing
    afe_config->aec_init = false;       // Disable AEC (saves ~50KB)
    afe_config->se_init = false;        // Disable speech enhancement (saves ~30KB)
    afe_config->vad_init = false;       // Disable VAD (saves ~10KB) - wake word has its own VAD
    afe_config->agc_init = false;       // Disable AGC (saves ~5KB) - rely on hardware gain
    afe_config->ns_init = false;        // Disable noise suppression (saves ~20KB)
    
    // CRITICAL MEMORY SETTINGS: Minimize buffer sizes
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;  // Use internal RAM
    afe_config->afe_ringbuf_size = 2;   // Minimal ringbuffer (down from 8) - saves ~24KB
    afe_config->afe_linear_gain = 1.0f;
    afe_config->afe_mode = AFE_MODE_LOW_COST;  // Ensure low-cost mode
    
    // Check and validate configuration
    afe_config = afe_config_check(afe_config);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE config check failed");
        return ESP_FAIL;
    }
    
    // Print configuration for debugging
    ESP_LOGI(TAG, "ESP-SR Memory Optimized Configuration:");
    ESP_LOGI(TAG, "  - Sample Rate: %d Hz", afe_config->pcm_config.sample_rate);
    ESP_LOGI(TAG, "  - Channels: %d", afe_config->pcm_config.total_ch_num);
    ESP_LOGI(TAG, "  - Ringbuffer Size: %d (minimal)", afe_config->afe_ringbuf_size);
    ESP_LOGI(TAG, "  - Wake Word Model: %s", afe_config->wakenet_model_name);
    ESP_LOGI(TAG, "  - Detection Mode: %d (conservative)", afe_config->wakenet_mode);
    ESP_LOGI(TAG, "  - AEC: %s, VAD: %s, AGC: %s, NS: %s", 
             afe_config->aec_init ? "ON" : "OFF",
             afe_config->vad_init ? "ON" : "OFF", 
             afe_config->agc_init ? "ON" : "OFF",
             afe_config->ns_init ? "ON" : "OFF");
    
    // Get AFE handle using the correct function
    esp_afe_sr_iface_t* iface = esp_afe_handle_from_config(afe_config);
    if (!iface || !iface->create_from_config) {
        ESP_LOGE(TAG, "Failed to get AFE interface from config");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    afe_handle_ = (void*)iface;
    
    // Create AFE data
    afe_data_ = (void*)iface->create_from_config(afe_config);
    if (!afe_data_) {
        ESP_LOGE(TAG, "Failed to create AFE data");
        afe_config_free(afe_config);
        return ESP_FAIL;
    }
    
    // Print pipeline for debugging
    iface->print_pipeline((esp_afe_sr_data_t*)afe_data_);
    
    // Clean up config as it's no longer needed
    afe_config_free(afe_config);
    
    ESP_LOGI(TAG, "ESP-SR AFE initialized successfully with memory optimizations");
    return ESP_OK;
}

void WakeWordDetector::cleanup_esp_sr() {
    ESP_LOGI(TAG, "Cleaning up ESP-SR");
    
    if (afe_handle_ && afe_data_) {
        esp_afe_sr_iface_t* iface = (esp_afe_sr_iface_t*)afe_handle_;
        if (iface && iface->destroy) {
            iface->destroy((esp_afe_sr_data_t*)afe_data_);
        }
        afe_data_ = nullptr;
    }
    
    afe_handle_ = nullptr;
    wake_word_models_ = nullptr;
}

} // namespace btspeaker 