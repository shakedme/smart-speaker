#pragma once

// Voice Assistant Configuration

// Gemini API Configuration
// You need to set your actual API key here or via environment variable
#ifndef CONFIG_GEMINI_API_KEY
#define CONFIG_GEMINI_API_KEY "your_gemini_api_key_here"
#endif

// Audio Configuration
#define CONFIG_MIC_SAMPLE_RATE      16000
#define CONFIG_MIC_CHANNELS         1
#define CONFIG_MIC_BITS_PER_SAMPLE  16

// Audio format definitions for I2S
#define RESPONSE_SAMPLE_RATE        24000
#define RESPONSE_CHANNELS           2
#define RESPONSE_BITS               I2S_DATA_BIT_WIDTH_16BIT
#define MIC_BITS                    I2S_DATA_BIT_WIDTH_16BIT

// Wake Word Configuration
#define CONFIG_WAKE_WORD_ENABLED    true
#define CONFIG_WAKE_WORD_THRESHOLD  0.8f

// Recording Configuration
#define CONFIG_MAX_RECORDING_MS     10000  // 10 seconds
#define CONFIG_AUDIO_BUFFER_SIZE    1024

// Network Configuration
#define CONFIG_GEMINI_TIMEOUT_MS    30000  // 30 seconds
#define CONFIG_MAX_RESPONSE_SIZE    (512 * 1024)  // 512KB

// WiFi Configuration (required for Gemini API)
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "Shaked"
#endif

#ifndef CONFIG_WIFI_PASSWORD  
#define CONFIG_WIFI_PASSWORD "0543512375"
#endif

// Hardware Pin Configuration (from hardware spec)
#define CONFIG_MIC_GPIO             35  // MAX9814 output connected to GPIO35 (ADC1_CH7)
#define CONFIG_I2S_BCK_GPIO         25  // Bit Clock
#define CONFIG_I2S_WS_GPIO          22  // Word Select  
#define CONFIG_I2S_DO_GPIO          26  // Data Out 