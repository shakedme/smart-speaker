#include "../include/wake_word_detector.hpp"
#include "../include/voice_config.hpp"
#include <algorithm>

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
    , processing_task_handle_(nullptr)
    , audio_ring_buffer_(nullptr)
    , enable_mutex_(nullptr)
    , task_running_(false)
    , afe_handle_(nullptr)
    , afe_data_(nullptr)
    , wake_word_models_(nullptr)
    , feed_chunksize_(0)
    , fetch_chunksize_(0)
{
    ESP_LOGI(TAG, "WakeWordDetector created");
}

WakeWordDetector::~WakeWordDetector() {
    shutdown();
}

esp_err_t WakeWordDetector::init() {
    ESP_LOGI(TAG, "Initializing wake word detector with dedicated processing task");
    
    // Create mutex for thread-safe enable/disable operations
    enable_mutex_ = xSemaphoreCreateMutex();
    if (!enable_mutex_) {
        ESP_LOGE(TAG, "Failed to create enable mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // ESP-SR AFE will process dual microphone data directly
    // No mono conversion buffer needed
    
    // Create ring buffer for audio data
    audio_ring_buffer_ = xRingbufferCreate(RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_ring_buffer_) {
        ESP_LOGE(TAG, "Failed to create audio ring buffer");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = setup_esp_sr();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR setup failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Wake word detection will be disabled");
        enabled_ = false;
        return ESP_OK; // Don't fail initialization
    }
    
    // Start the processing task
    ret = start_processing_task();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wake word processing task");
        cleanup_esp_sr();
        return ret;
    }
    
    enabled_ = CONFIG_WAKE_WORD_ENABLED;
    ESP_LOGI(TAG, "Wake word detector initialized with task (enabled: %s)", enabled_ ? "yes" : "no");
    return ESP_OK;
}

void WakeWordDetector::shutdown() {
    ESP_LOGI(TAG, "Shutting down wake word detector");
    
    set_enabled(false);
    stop_processing_task();
    cleanup_esp_sr();
    
    if (audio_ring_buffer_) {
        vRingbufferDelete(audio_ring_buffer_);
        audio_ring_buffer_ = nullptr;
    }
    
    // No mono conversion buffer to clean up
    
    if (enable_mutex_) {
        vSemaphoreDelete(enable_mutex_);
        enable_mutex_ = nullptr;
    }
}

esp_err_t WakeWordDetector::feed_audio_data(const uint8_t* stereo_data, size_t data_length) {
    if (!enabled_ || !stereo_data || data_length == 0) {
        return ESP_OK; // Silently ignore when disabled
    }
    
    if (!audio_ring_buffer_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Send stereo data to ring buffer for processing by the dedicated task
    // The processing task will handle stereo-to-mono conversion
    BaseType_t result = xRingbufferSend(audio_ring_buffer_, stereo_data, data_length, 0);
    if (result != pdTRUE) {
        ESP_LOGD(TAG, "Ring buffer full, dropping audio data");
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

void WakeWordDetector::set_enabled(bool enabled) {
    if (enable_mutex_ && xSemaphoreTake(enable_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (enabled_ != enabled) {
            enabled_ = enabled;
            ESP_LOGI(TAG, "Wake word detection %s", enabled ? "enabled" : "disabled");
            
            if (!enabled && audio_ring_buffer_) {
                // Clear ring buffer when disabling
                size_t bytes_available;
                void* data = xRingbufferReceive(audio_ring_buffer_, &bytes_available, 0);
                while (data) {
                    vRingbufferReturnItem(audio_ring_buffer_, data);
                    data = xRingbufferReceive(audio_ring_buffer_, &bytes_available, 0);
                }
            }
        }
        xSemaphoreGive(enable_mutex_);
    } else {
        // Fallback without mutex
        enabled_ = enabled;
        ESP_LOGW(TAG, "Wake word detection %s (without mutex)", enabled ? "enabled" : "disabled");
    }
}

esp_err_t WakeWordDetector::start_processing_task() {
    if (processing_task_handle_) {
        ESP_LOGW(TAG, "Processing task already running");
        return ESP_OK;
    }
    
    task_running_ = true;
    BaseType_t result = xTaskCreate(
        wake_word_task,
        "wakeword_task",
        WAKE_WORD_TASK_STACK_SIZE,
        this,
        WAKE_WORD_TASK_PRIORITY,
        &processing_task_handle_
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake word processing task");
        task_running_ = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Wake word processing task started");
    return ESP_OK;
}

void WakeWordDetector::stop_processing_task() {
    if (!processing_task_handle_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping wake word processing task");
    task_running_ = false;
    
    // Wait a bit for task to finish current processing
    vTaskDelay(pdMS_TO_TICKS(100));
    
    vTaskDelete(processing_task_handle_);
    processing_task_handle_ = nullptr;

    ESP_LOGI(TAG, "Wake word processing task stopped");
}

void WakeWordDetector::wake_word_task(void* arg) {
    auto* detector = static_cast<WakeWordDetector*>(arg);
    detector->run_wake_word_processing();
    vTaskDelete(NULL);
}

void WakeWordDetector::run_wake_word_processing() {
    ESP_LOGI(TAG, "Wake word processing task started (dual microphone mode)");
    
    while (task_running_) {
        if (!enabled_ || !audio_ring_buffer_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Receive stereo audio data from ring buffer
        size_t data_length;
        auto* stereo_data = (uint8_t*)xRingbufferReceive(audio_ring_buffer_, &data_length, pdMS_TO_TICKS(50));
        
        if (!stereo_data) {
            continue; // Timeout, check again
        }
        
        // Convert raw bytes to 16-bit samples for ESP-SR processing
        const auto* stereo_samples = (const int16_t*)stereo_data;
        size_t sample_count = data_length / sizeof(int16_t);
        
        if (sample_count > 0 && (sample_count % 2 == 0)) { // Ensure we have pairs of samples
            bool wake_word_detected = process_stereo_audio_chunk(stereo_samples, sample_count / 2); // /2 because we want frame count, not sample count
            
            if (wake_word_detected && wake_word_callback_) {
                ESP_LOGI(TAG, "Calling wake word callback");
                wake_word_callback_();
            }
        }
        
        // Return the buffer
        vRingbufferReturnItem(audio_ring_buffer_, stereo_data);
    }
    
    ESP_LOGI(TAG, "Wake word processing task ended");
}

bool WakeWordDetector::process_stereo_audio_chunk(const int16_t* stereo_samples, size_t frame_count) {
    if (!enabled_ || !stereo_samples || frame_count == 0) {
        return false;
    }
    
    if (!afe_handle_ || !afe_data_) {
        return false;
    }
    
    auto* iface = (esp_afe_sr_iface_t*)afe_handle_;
    
    // Process audio in proper chunk sizes expected by AFE
    size_t frames_processed = 0;
    while (frames_processed < frame_count) {
        size_t frames_to_process = std::min(frame_count - frames_processed, 
                                          static_cast<size_t>(feed_chunksize_));
        
        // Create temporary buffer for this chunk (2 channels * frames * sizeof(int16_t))
        // ESP-SR AFE expects interleaved stereo: L0,R0,L1,R1,L2,R2...
        const int16_t* chunk_data = stereo_samples + (frames_processed * 2);
        
        // Feed stereo audio data to ESP-SR AFE
        // The AFE configured with "MM" format expects this exact dual-channel layout
        int feed_result = iface->feed((esp_afe_sr_data_t*)afe_data_, (int16_t*)chunk_data);
        
        if (feed_result <= 0) {
            ESP_LOGD(TAG, "AFE feed failed: %d", feed_result);
            frames_processed += frames_to_process;
            continue;
        }
        
        // Fetch result from AFE
        afe_fetch_result_t* fetch_result = iface->fetch((esp_afe_sr_data_t*)afe_data_);
        if (!fetch_result) {
            frames_processed += frames_to_process;
            continue;
        }
        
        // Check for wake word detection
        if (fetch_result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected with dual microphone processing!");
            ESP_LOGI(TAG, "  - Model Index: %d", fetch_result->wakenet_model_index);
            ESP_LOGI(TAG, "  - Word Index: %d", fetch_result->wake_word_index);
            ESP_LOGI(TAG, "  - Trigger Channel: %d", fetch_result->trigger_channel_id);
            ESP_LOGI(TAG, "  - Beamforming enabled: spatial audio processing active");

            return true;
        }
        
        frames_processed += frames_to_process;
    }
    
    return false;
}

void WakeWordDetector::set_wake_word_callback(WakeWordCallback callback) {
    wake_word_callback_ = callback;
}

esp_err_t WakeWordDetector::setup_esp_sr() {
    ESP_LOGI(TAG, "Setting up ESP-SR with dual microphone configuration");
    
    // For compiled-in models (enabled via menuconfig), use NULL instead of partition name
    srmodel_list_t *models = esp_srmodel_init(NULL);
    if (!models) {
        ESP_LOGW(TAG, "No compiled-in SR models found. Please enable wake word models in menuconfig:");
        ESP_LOGW(TAG, "Component config -> ESP Speech Recognition -> Wake Word Engine -> Enable");
        ESP_LOGW(TAG, "Component config -> ESP Speech Recognition -> Wake Word Engine -> Select wn9_hiesp for ESP32");
        return ESP_FAIL;
    }

    // Create AFE configuration for dual microphone processing
    // Format "MM": Two microphone channels (M=microphone), no reference channel
    // This enables beamforming and spatial processing with the two MEMS mics
    afe_config_t *afe_config = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "Failed to create AFE config");
        return ESP_FAIL;
    }
    
    // Configure for our dual MEMS microphone hardware setup
    afe_config->pcm_config.sample_rate = CONFIG_MIC_SAMPLE_RATE;  // 16kHz
    afe_config->pcm_config.total_ch_num = 2;  // Two microphones total
    afe_config->pcm_config.mic_num = 2;       // Two microphones for beamforming
    afe_config->pcm_config.ref_num = 0;       // No reference channel (could add speaker reference later for AEC)
    
    // Wake word detection configuration
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = const_cast<char*>("wn9_hiesp");  // Use WakeNet5 "Hi ESP" for ESP32
    afe_config->wakenet_mode = DET_MODE_90;  // Balanced detection sensitivity
    
    // Audio processing pipeline configuration for smart speaker with music playback
    afe_config->se_init = true;         // Enable Speech Enhancement (includes beamforming with MM format)
    afe_config->vad_init = true;        // Enable VAD for better voice detection
    afe_config->agc_init = false;       // Disable AGC - rely on hardware mic gain
    afe_config->ns_init = true;         // Enable noise suppression for background music
    afe_config->ns_model_name = const_cast<char*>("WEBRTC");  // Use WebRTC noise suppression
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    
    // AEC configuration - we don't have speaker reference channel yet, so disable for now
    // TODO: Add speaker reference channel connection for full AEC capability
    afe_config->aec_init = false;       // Disable AEC (need reference channel)
    
    // VAD configuration for music environment
    afe_config->vad_mode = VAD_MODE_3;  // More aggressive VAD for noisy environments
    afe_config->vad_min_speech_ms = 96; // Shorter minimum speech duration for responsiveness
    afe_config->vad_min_noise_ms = 500; // Shorter noise duration for music environments
    afe_config->vad_delay_ms = 96;      // Reduced delay for faster response
    
    // Memory and performance configuration
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;  // Use PSRAM for dual-mic processing
    afe_config->afe_ringbuf_size = 6;   // Larger buffer for dual microphone processing
    afe_config->afe_linear_gain = 1.0f; // No additional digital gain
    afe_config->afe_mode = AFE_MODE_LOW_COST;  // Low cost mode for ESP32
    
    // Check and validate configuration
    afe_config = afe_config_check(afe_config);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE config check failed");
        return ESP_FAIL;
    }
    
    // Print configuration details
    ESP_LOGI(TAG, "ESP-SR AFE Configuration:");
    ESP_LOGI(TAG, "  Format: MM (dual microphone)");
    ESP_LOGI(TAG, "  Sample Rate: %d Hz", afe_config->pcm_config.sample_rate);
    ESP_LOGI(TAG, "  Total Channels: %d", afe_config->pcm_config.total_ch_num);
    ESP_LOGI(TAG, "  Microphones: %d", afe_config->pcm_config.mic_num);
    ESP_LOGI(TAG, "  Speech Enhancement: %s", afe_config->se_init ? "enabled" : "disabled");
    ESP_LOGI(TAG, "  VAD: %s", afe_config->vad_init ? "enabled" : "disabled");
    ESP_LOGI(TAG, "  Noise Suppression: %s", afe_config->ns_init ? "enabled" : "disabled");
    ESP_LOGI(TAG, "  AEC: %s", afe_config->aec_init ? "enabled" : "disabled");
    if (afe_config->wakenet_model_name) {
        ESP_LOGI(TAG, "  Wake Word Model: %s", afe_config->wakenet_model_name);
    }
    
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
    
    // Get proper chunk sizes for audio processing
    feed_chunksize_ = iface->get_feed_chunksize((esp_afe_sr_data_t*)afe_data_);
    fetch_chunksize_ = iface->get_fetch_chunksize((esp_afe_sr_data_t*)afe_data_);
    
    ESP_LOGI(TAG, "AFE Processing Configuration:");
    ESP_LOGI(TAG, "  Feed Chunk Size: %d samples", feed_chunksize_);
    ESP_LOGI(TAG, "  Fetch Chunk Size: %d samples", fetch_chunksize_);
    ESP_LOGI(TAG, "  Expected Input: %d channels, 16-bit samples", afe_config->pcm_config.total_ch_num);
    
    // Print the processing pipeline
    iface->print_pipeline((esp_afe_sr_data_t*)afe_data_);
    
    afe_config_free(afe_config);
    
    ESP_LOGI(TAG, "ESP-SR AFE initialized with dual microphone beamforming and noise suppression");
    return ESP_OK;
}

void WakeWordDetector::cleanup_esp_sr() {
    ESP_LOGI(TAG, "Cleaning up ESP-SR");
    
    if (afe_handle_ && afe_data_) {
        auto* iface = (esp_afe_sr_iface_t*)afe_handle_;
        if (iface && iface->destroy) {
            iface->destroy((esp_afe_sr_data_t*)afe_data_);
        }
        afe_data_ = nullptr;
    }
    
    afe_handle_ = nullptr;
    wake_word_models_ = nullptr;
    feed_chunksize_ = 0;
    fetch_chunksize_ = 0;
}

} // namespace btspeaker 