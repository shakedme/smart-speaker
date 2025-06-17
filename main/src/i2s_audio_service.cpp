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
    
    if (isInitialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing I2S Audio Service");
    
    // Initialize state
    activeSource_ = AudioSource::NONE;
    masterVolume_ = DEFAULT_VOLUME;
    i2SDriverInstalled_ = false;
    currentSampleRate_ = DEFAULT_SAMPLE_RATE;
    currentChannels_ = DEFAULT_CHANNELS;
    currentBitsPerSample_ = DEFAULT_BITS;

    isInitialized_ = true;
    ESP_LOGI(TAG, "I2S Audio Service initialized (driver will be installed on first use)");
    return ESP_OK;
}

esp_err_t I2SAudioService::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!isInitialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Shutting down I2S Audio Service");
    
    // Deinitialize I2S driver if it was installed
    if (i2SDriverInstalled_) {
        deinitializeI2SDriver();
    }

    activeSource_ = AudioSource::NONE;
    isInitialized_ = false;
    
    ESP_LOGI(TAG, "I2S Audio Service shut down");
    return ESP_OK;
}

audio_element_handle_t I2SAudioService::createI2SWriter(AudioSource source, 
                                                       int sample_rate,
                                                       int channels, 
                                                       int bits_per_sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!isInitialized_) {
        ESP_LOGE(TAG, "Service not initialized");
        return nullptr;
    }
    
    ESP_LOGI(TAG, "Creating I2S writer for source %d (%dHz, %dch, %dbit)", 
             static_cast<int>(source), sample_rate, channels, bits_per_sample);
    
    // Create I2S stream configuration
    i2s_stream_cfg_t i2SConfig = createI2SConfig(source, sample_rate, channels, bits_per_sample);
    
    // Create the I2S stream element
    audio_element_handle_t i2SHandle = i2s_stream_init(&i2SConfig);
    if (!i2SHandle) {
        ESP_LOGE(TAG, "Failed to create I2S stream for source %d", static_cast<int>(source));
        return nullptr;
    }
    
    ESP_LOGI(TAG, "Created I2S writer for source %d", static_cast<int>(source));
    return i2SHandle;
}

esp_err_t I2SAudioService::setActiveSource(AudioSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!isInitialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (activeSource_ == source) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Switching active source from %d to %d",
             static_cast<int>(activeSource_), static_cast<int>(source));

    activeSource_ = source;
    return ESP_OK;
}

esp_err_t I2SAudioService::deinitializeI2SDriver() {
    // This is called from within a locked context
    if (!i2SDriverInstalled_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing I2S driver");
    // Note: I2S driver cleanup is handled by individual pipeline cleanup
    i2SDriverInstalled_ = false;
    return ESP_OK;
}

i2s_stream_cfg_t I2SAudioService::createI2SConfig(AudioSource source, 
                                                  int sampleRate,
                                                  int channels, 
                                                  int bitsPerSample) {
    // Determine source-specific configuration
    int taskPriority = 23; // Default
    int outRbSize = 12 * 1024; // Default
    bool uninstallDrv = false; // Shared I2S, don't uninstall
    
    switch (source) {
        case AudioSource::BLUETOOTH:
            taskPriority = 23;
            outRbSize = 12 * 1024;
            uninstallDrv = false; // Share with other sources
            break;
        case AudioSource::VOICE_RESPONSE:
            taskPriority = 25; // Higher priority for voice
            outRbSize = 4 * 1024;
            uninstallDrv = false;
            break;
        default:
            break;
    }
    
    // Create standardized I2S configuration using board.h pins
    i2s_stream_cfg_t i2SCfg = {
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
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sampleRate)),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                static_cast<i2s_data_bit_width_t>(bitsPerSample),
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
        .volume = masterVolume_,
        .out_rb_size = outRbSize,
        .task_stack = 3072,
        .task_core = 0,
        .task_prio = taskPriority,
        .stack_in_ext = false,
        .multi_out_num = 0,
        .uninstall_drv = uninstallDrv,
        .need_expand = false,
        .expand_src_bits = static_cast<i2s_data_bit_width_t>(bitsPerSample),
        .buffer_len = 2400
    };
    
    ESP_LOGD(TAG, "Created I2S config for source %d: %dHz, %dch, %dbit, prio=%d",
             static_cast<int>(source), sampleRate, channels, bitsPerSample, taskPriority);
    
    return i2SCfg;
}

} // namespace btspeaker 