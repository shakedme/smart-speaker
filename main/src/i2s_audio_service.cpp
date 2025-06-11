#include "../include/i2s_audio_service.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_check.h"
#include "audio_common.h"
}

namespace btspeaker {

I2SAudioService& I2SAudioService::getInstance() {
    static I2SAudioService instance;
    return instance;
}

esp_err_t I2SAudioService::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_initialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing I2S Audio Service");
    
    // Initialize state
    active_source_ = AudioSource::NONE;
    master_volume_ = DEFAULT_VOLUME;
    i2s_driver_installed_ = false;
    current_sample_rate_ = DEFAULT_SAMPLE_RATE;
    current_channels_ = DEFAULT_CHANNELS;
    current_bits_per_sample_ = DEFAULT_BITS;
    
    is_initialized_ = true;
    ESP_LOGI(TAG, "I2S Audio Service initialized (driver will be installed on first use)");
    return ESP_OK;
}

esp_err_t I2SAudioService::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_initialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Shutting down I2S Audio Service");
    
    // Deinitialize I2S driver if it was installed
    if (i2s_driver_installed_) {
        deinitializeI2SDriver();
    }
    
    active_source_ = AudioSource::NONE;
    is_initialized_ = false;
    
    ESP_LOGI(TAG, "I2S Audio Service shut down");
    return ESP_OK;
}

audio_element_handle_t I2SAudioService::createI2SWriter(AudioSource source, 
                                                       int sample_rate,
                                                       int channels, 
                                                       int bits_per_sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_initialized_) {
        ESP_LOGE(TAG, "Service not initialized");
        return nullptr;
    }
    
    ESP_LOGI(TAG, "Creating I2S writer for source %d (%dHz, %dch, %dbit)", 
             static_cast<int>(source), sample_rate, channels, bits_per_sample);
    
    // Create I2S stream configuration
    i2s_stream_cfg_t i2s_cfg = createI2SConfig(source, sample_rate, channels, bits_per_sample);
    
    // Create the I2S stream element
    audio_element_handle_t i2s_handle = i2s_stream_init(&i2s_cfg);
    if (!i2s_handle) {
        ESP_LOGE(TAG, "Failed to create I2S stream for source %d", static_cast<int>(source));
        return nullptr;
    }
    
    ESP_LOGI(TAG, "Created I2S writer for source %d", static_cast<int>(source));
    return i2s_handle;
}

esp_err_t I2SAudioService::setActiveSource(AudioSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (active_source_ == source) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Switching active source from %d to %d", 
             static_cast<int>(active_source_), static_cast<int>(source));
    
    active_source_ = source;
    return ESP_OK;
}

esp_err_t I2SAudioService::updateI2SFormat(int sample_rate, int bits_per_sample, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if format actually changed
    if (current_sample_rate_ == sample_rate && 
        current_bits_per_sample_ == bits_per_sample && 
        current_channels_ == channels) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Updating I2S format: %dHz, %dbit, %dch", sample_rate, bits_per_sample, channels);
    
    // Update current format
    current_sample_rate_ = sample_rate;
    current_bits_per_sample_ = bits_per_sample;
    current_channels_ = channels;
    
    // Note: Individual pipelines will handle their own I2S stream updates
    // This service tracks the current format for new stream creation
    
    return ESP_OK;
}

void I2SAudioService::setMasterVolume(uint8_t volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    master_volume_ = volume;
    ESP_LOGI(TAG, "Master volume set to %d", volume);
}

bool I2SAudioService::isI2SActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_initialized_ && active_source_ != AudioSource::NONE;
}

esp_err_t I2SAudioService::initializeI2SDriver() {
    // This is called from within a locked context
    if (i2s_driver_installed_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Installing I2S driver");
    // Note: I2S driver installation is handled by individual i2s_stream_init calls
    // This service coordinates but doesn't directly manage the low-level driver
    i2s_driver_installed_ = true;
    return ESP_OK;
}

esp_err_t I2SAudioService::deinitializeI2SDriver() {
    // This is called from within a locked context
    if (!i2s_driver_installed_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing I2S driver");
    // Note: I2S driver cleanup is handled by individual pipeline cleanup
    i2s_driver_installed_ = false;
    return ESP_OK;
}

i2s_stream_cfg_t I2SAudioService::createI2SConfig(AudioSource source, 
                                                  int sample_rate, 
                                                  int channels, 
                                                  int bits_per_sample) {
    // This is called from within a locked context
    
    // Determine source-specific configuration
    int task_priority = 23; // Default
    int out_rb_size = 12 * 1024; // Default
    bool uninstall_drv = false; // Shared I2S, don't uninstall
    
    switch (source) {
        case AudioSource::BLUETOOTH:
            task_priority = 23;
            out_rb_size = 12 * 1024;
            uninstall_drv = false; // Share with other sources
            break;
        case AudioSource::SPOTIFY:
            task_priority = 24;
            out_rb_size = 8 * 1024;
            uninstall_drv = false;
            break;
        case AudioSource::VOICE_RESPONSE:
            task_priority = 25; // Higher priority for voice
            out_rb_size = 4 * 1024;
            uninstall_drv = false;
            break;
        default:
            break;
    }
    
    // Create standardized I2S configuration using board.h pins
    i2s_stream_cfg_t i2s_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .transmit_mode = I2S_COMM_MODE_STD,
        .chan_cfg = {
            .id = static_cast<i2s_port_t>(I2S_PORT),
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 4,
            .dma_frame_num = 160,
            .auto_clear = true,
            .auto_clear_before_cb = false,
            .allow_pd = false,
            .intr_priority = 0
        },
        .std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sample_rate)),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                static_cast<i2s_data_bit_width_t>(bits_per_sample),
                static_cast<i2s_slot_mode_t>(channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO)
            ),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = static_cast<gpio_num_t>(I2S_BCK_IO), // GPIO25 from board.h
                .ws   = static_cast<gpio_num_t>(I2S_WS_IO),  // GPIO22 from board.h
                .dout = static_cast<gpio_num_t>(I2S_DO_IO),  // GPIO26 from board.h
                .din  = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false
                }
            },
        },
#if SOC_I2S_SUPPORTS_PDM_RX
        .pdm_rx_cfg = {},
#endif
#if SOC_I2S_SUPPORTS_PDM_TX
        .pdm_tx_cfg = {},
#endif
        .use_alc = false,
        .volume = master_volume_,
        .out_rb_size = out_rb_size,
        .task_stack = 3072,
        .task_core = 0,
        .task_prio = task_priority,
        .stack_in_ext = false,
        .multi_out_num = 0,
        .uninstall_drv = uninstall_drv,
        .need_expand = false,
        .expand_src_bits = static_cast<i2s_data_bit_width_t>(bits_per_sample),
        .buffer_len = 2400
    };
    
    ESP_LOGD(TAG, "Created I2S config for source %d: %dHz, %dch, %dbit, prio=%d", 
             static_cast<int>(source), sample_rate, channels, bits_per_sample, task_priority);
    
    return i2s_cfg;
}

} // namespace btspeaker 