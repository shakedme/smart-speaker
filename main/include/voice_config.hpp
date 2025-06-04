#pragma once

// Voice Assistant Configuration

// WebSocket Server Configuration
#ifndef CONFIG_WEBSOCKET_SERVER_URL
#define CONFIG_WEBSOCKET_SERVER_URL "ws://localhost:8080/voice"
#endif

// WebSocket Connection Settings
#define CONFIG_WS_KEEPALIVE_INTERVAL_S  30
#define CONFIG_WS_PING_INTERVAL_S       10
#define CONFIG_WS_CHUNK_SIZE            4096
#define CONFIG_WS_RECONNECT_TIMEOUT_MS  5000
#define CONFIG_WS_MAX_RECONNECT_ATTEMPTS 5

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

// Recording Configuration (for streaming chunks)
#define CONFIG_MAX_RECORDING_MS     10000  // 10 seconds max session
#define CONFIG_AUDIO_BUFFER_SIZE    CONFIG_WS_CHUNK_SIZE  // Use WebSocket chunk size

// Network Configuration
#define CONFIG_CONNECTION_TIMEOUT_MS 10000  // 10 seconds
#define CONFIG_STREAM_TIMEOUT_MS     30000  // 30 seconds for streaming

// WiFi Configuration (required for WebSocket connection)
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