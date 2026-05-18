#pragma once

// WebSocket Server Configuration
#define CONFIG_WEBSOCKET_SERVER_URL "ws://192.168.1.124:8080/voice"

// WebSocket Connection Settings
#define CONFIG_WS_KEEPALIVE_INTERVAL_S  30
#define CONFIG_WS_PING_INTERVAL_S       10
#define CONFIG_WS_CHUNK_SIZE            2048 
#define CONFIG_WS_RECONNECT_TIMEOUT_MS  1000
#define CONFIG_WS_MAX_RECONNECT_ATTEMPTS 15

// Audio Configuration - Updated for SPH0645LM4H I2S MEMS microphones
#define CONFIG_MIC_SAMPLE_RATE      48000
#define MIC_BITS                    I2S_DATA_BIT_WIDTH_24BIT

// WiFi Configuration (required for WebSocket connection)
#define CONFIG_WIFI_SSID "Shaked"
#define CONFIG_WIFI_PASSWORD "0543512375"

// Hardware Pin Configuration - Updated for single SPH0645LM4H I2S MEMS microphone
// Note: Using I2S1 for microphone to avoid conflict with I2S0 used for audio output (PCM5102A)
// Wire SPH0645LM4H: SEL->GND, LRCL->GPIO33, DOUT->GPIO34, BCLK->GPIO32, 3V->3.3V, GND->GND
#define CONFIG_MIC_I2S_BCK_GPIO     GPIO_NUM_32  // I2S1 Bit Clock for microphone
#define CONFIG_MIC_I2S_WS_GPIO      GPIO_NUM_33  // I2S1 Word Select for microphone  
#define CONFIG_MIC_I2S_DIN_GPIO     GPIO_NUM_34  // I2S1 Data In from microphone