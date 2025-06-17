#pragma once

// Voice Assistant Configuration

// WebSocket Server Configuration
#ifndef CONFIG_WEBSOCKET_SERVER_URL
#define CONFIG_WEBSOCKET_SERVER_URL "ws://a8ca-2a00-a041-e530-c500-5dfb-2883-2eeb-557b.ngrok-free.app/voice"
#endif

// WebSocket Connection Settings
#define CONFIG_WS_KEEPALIVE_INTERVAL_S  30
#define CONFIG_WS_PING_INTERVAL_S       10
#define CONFIG_WS_CHUNK_SIZE            2048 
#define CONFIG_WS_RECONNECT_TIMEOUT_MS  5000
#define CONFIG_WS_MAX_RECONNECT_ATTEMPTS 5

// Audio Configuration - Updated for SPH0645LM4H I2S MEMS microphones
#define CONFIG_MIC_SAMPLE_RATE      16000
#define MIC_BITS                    I2S_DATA_BIT_WIDTH_16BIT

// Wake Word Configuration
#define CONFIG_WAKE_WORD_ENABLED    true
#define CONFIG_WAKE_WORD_THRESHOLD  0.8f

// WiFi Configuration (required for WebSocket connection)
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "Shaked"
#endif

#ifndef CONFIG_WIFI_PASSWORD  
#define CONFIG_WIFI_PASSWORD "0543512375"
#endif

// Hardware Pin Configuration - Updated for SPH0645LM4H I2S MEMS microphones
// Note: Using I2S1 for microphones to avoid conflict with I2S0 used for audio output (PCM5102A)
#define CONFIG_MIC_I2S_BCK_GPIO     GPIO_NUM_32  // I2S1 Bit Clock for microphones
#define CONFIG_MIC_I2S_WS_GPIO      GPIO_NUM_33  // I2S1 Word Select for microphones  
#define CONFIG_MIC_I2S_DIN_GPIO     GPIO_NUM_34  // I2S1 Data In from microphones