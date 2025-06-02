# Voice Assistant Integration for ESP32 Smart Speaker

This implementation adds comprehensive voice assistant capabilities to your ESP32 smart speaker using Google's Gemini 2.0 Flash audio-to-audio API, ESP-SR wake word detection, and seamless integration with your existing Bluetooth audio system.

## Features

### 🎤 **Complete Audio-to-Audio Flow**
- **Microphone Input**: MAX9814 microphone module → GPIO35 (ADC) → ESP32
- **Wake Word Detection**: ESP-SR powered wake word recognition
- **Voice Command Processing**: Direct audio-to-audio communication with Gemini API
- **Audio Response**: Gemini response → I2S → PCM5102A → TPA3116D2 → Speakers
- **Music Control**: Voice commands for play/pause/stop/restart

### 🧠 **Smart Audio Management**
- **Dual Pipeline Architecture**: Separate pipelines for Bluetooth audio and voice responses
- **Automatic Audio Switching**: Temporarily pauses Bluetooth when playing voice responses
- **Seamless Integration**: Returns to Bluetooth audio after voice interaction
- **Music Command Detection**: Local processing for immediate music control

### 🌐 **Cloud Integration**
- **WiFi Connectivity**: Automatic connection for internet access
- **Gemini API**: Google's latest audio-to-audio model (gemini-2.0-flash-exp)
- **Graceful Degradation**: Bluetooth audio works without internet, voice assistant requires WiFi

## Hardware Requirements

Based on your existing hardware specification:

### Audio Components
- **ESP32-DevKitC-32E** (ESP32-WROOM-32E module)
- **PCM5102A I2S DAC Module** (audio output)
- **MAX9814 Microphone Module** (audio input)
- **TPA3116D2 Amplifier** (Sure Electronics AA-JA32172)
- **Speakers**: 2× Dayton Audio RS100P-4 (4Ω, 30W)

### Pin Connections
```
ESP32 → PCM5102A (I2S Audio Output):
├── GPIO22 → LCK (Word Select)
├── GPIO25 → BCK (Bit Clock) 
├── GPIO26 → DIN (Data Input)
├── 5V → VCC
└── GND → GND

ESP32 → MAX9814 (Microphone Input):
├── GPIO35 (ADC1_CH7) → OUT (Analog Audio)
├── 5V → VCC
└── GND → GND

Audio Signal Chain:
Microphone → MAX9814 → ESP32 → [Processing] → PCM5102A → TPA3116D2 → Speakers
```

## Software Setup

### 1. **Configuration**

Edit `main/include/voice_config.hpp`:

```cpp
// Required: Set your Gemini API key
#define CONFIG_GEMINI_API_KEY "your_actual_api_key_here"

// Required: Set your WiFi credentials  
#define CONFIG_WIFI_SSID "your_wifi_network"
#define CONFIG_WIFI_PASSWORD "your_wifi_password"

// Optional: Adjust audio settings
#define CONFIG_MIC_SAMPLE_RATE      16000
#define CONFIG_MAX_RECORDING_MS     10000  // 10 seconds max
#define CONFIG_WAKE_WORD_THRESHOLD  0.8f
```

### 2. **Get Gemini API Key**

1. Go to [Google AI Studio](https://aistudio.google.com/)
2. Create a new project or select existing
3. Navigate to "Get API key" 
4. Create new API key
5. Copy the key to `CONFIG_GEMINI_API_KEY`

### 3. **ESP-IDF Configuration**

Update `sdkconfig` to enable required components:

```bash
# Enable ESP-SR (Speech Recognition)
CONFIG_AFE_INTERFACE_V1=y
CONFIG_SR_NSN_WEBRTC=y
CONFIG_SR_VADN_WEBRTC=y

# WiFi & Networking
CONFIG_ESP32_WIFI_ENABLED=y
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_IPV6=y

# HTTP Client for Gemini API
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y

# Audio Framework
CONFIG_AUDIO_PIPELINE=y
CONFIG_AUDIO_ELEMENT=y
```

### 4. **Build and Flash**

```bash
# Clean previous build
idf.py clean

# Configure (if needed)
idf.py menuconfig

# Build
idf.py build

# Flash to ESP32
idf.py flash monitor
```

## Usage Guide

### **Initial Setup**
1. Power on the ESP32 smart speaker
2. Wait for WiFi connection (check logs)
3. Wait for "Voice assistant started successfully" message
4. The system is now ready for voice commands

### **Voice Interaction Flow**

#### **1. Wake Word Detection**
- Say the configured wake word (default ESP-SR wake word)
- Look for "Wake word detected!" in logs
- LED indicator (if connected) should show activation

#### **2. Voice Commands**

**Music Control Commands** (processed locally):
```
"Play music"           → Resumes Bluetooth audio
"Pause music"          → Pauses current audio  
"Stop music"           → Stops audio playback
"Start over"           → Restarts current track
"Restart music"        → Restarts audio pipeline
```

**General Queries** (sent to Gemini):
```
"What's the weather like?"
"Tell me a joke"
"What time is it?"
"How do I cook pasta?"
```

#### **3. Audio Response**
- Gemini's audio response plays through speakers
- Bluetooth audio automatically pauses during response
- Returns to Bluetooth audio when response completes

### **System States**

The voice assistant operates in several states:

```
LISTENING          → Waiting for wake word
WAKE_WORD_DETECTED → Wake word recognized, starting recording
RECORDING_COMMAND  → Recording user command (max 10 seconds)
PROCESSING_GEMINI  → Sending audio to Gemini API
PLAYING_RESPONSE   → Playing Gemini's audio response
```

## Architecture Overview

### **Component Structure**
```
VoiceAssistantManager
├── MicrophonePipeline (I2S1 + Raw Stream)
├── WakeWordDetection (ESP-SR)
├── GeminiApiClient (HTTP + JSON + Base64)
├── AudioResponsePlayer (Raw → I2S0)
└── AudioPipelineManager (Bluetooth integration)
```

### **Audio Flow Diagram**
```
┌─────────────┐    ┌──────────────┐    ┌─────────────┐
│   MAX9814   │───▶│    ESP32     │───▶│ PCM5102A    │
│ Microphone  │    │ Processing   │    │    DAC      │
└─────────────┘    └──────────────┘    └─────────────┘
                           │                    │
                           ▼                    ▼
                   ┌──────────────┐    ┌─────────────┐
                   │ Gemini API   │    │ TPA3116D2   │
                   │   (Cloud)    │    │ Amplifier   │
                   └──────────────┘    └─────────────┘
                                              │
                                              ▼
                                     ┌─────────────┐
                                     │  Speakers   │
                                     └─────────────┘
```

### **Memory Usage**
- **Voice Assistant Task**: 8KB stack
- **Audio Response Task**: 4KB stack  
- **Recording Buffer**: ~320KB (10 seconds @ 16kHz)
- **Audio Queues**: ~10KB total
- **HTTP Client Buffer**: 512KB max response

## Troubleshooting

### **Common Issues**

#### **Voice Assistant Not Starting**
```
Check logs for:
- WiFi connection status
- Gemini API key validity
- ESP-SR initialization
- Memory allocation errors
```

#### **Wake Word Not Detected**
```
Verify:
- MAX9814 connections (GPIO35, VCC, GND)
- Microphone sensitivity (check audio levels)
- ESP-SR configuration
- Background noise levels
```

#### **No Audio Response**
```
Check:
- Internet connectivity
- Gemini API quotas/limits
- Audio pipeline conflicts
- I2S configuration
```

#### **Poor Audio Quality**
```
Adjust:
- Sample rates (16kHz input, 24kHz output)
- Buffer sizes
- DMA configurations
- Audio gain settings
```

### **Debug Commands**

Monitor system status:
```bash
# Check WiFi status
idf.py monitor | grep "WIFI"

# Monitor voice assistant states  
idf.py monitor | grep "VOICE_ASSISTANT"

# Check Gemini API calls
idf.py monitor | grep "GEMINI_API"

# Monitor audio pipeline
idf.py monitor | grep "PIPELINE"
```

## Performance Optimization

### **Memory Management**
- Recording buffer allocated only once at startup
- Audio queues use efficient ring buffers
- Automatic cleanup of temporary allocations

### **Real-Time Performance**
- Voice assistant runs on dedicated core
- High priority tasks for audio processing
- Minimal latency audio switching

### **Power Consumption**
- Wake word detection optimized for low power
- Audio pipelines suspend when inactive
- WiFi power management enabled

## Future Enhancements

### **Planned Features**
- [ ] Multiple wake word support
- [ ] Custom wake word training
- [ ] Streaming audio responses (real-time)
- [ ] Voice command customization
- [ ] Multi-language support
- [ ] Offline voice processing

### **Integration Possibilities**
- [ ] Smart home device control
- [ ] Calendar and reminder integration
- [ ] Music streaming service APIs
- [ ] Voice-controlled settings
- [ ] Personalized responses

## API Reference

### **VoiceAssistantManager**
```cpp
esp_err_t init(AudioPipelineManager* pipeline);
esp_err_t start();
esp_err_t stop();
void set_music_playback_callback(MusicPlaybackCallback callback);
State get_state() const;
```

### **GeminiApiClient**
```cpp
esp_err_t init();
esp_err_t send_audio_request(const uint8_t* audio_data, 
                           size_t length,
                           AudioResponseCallback callback);
void cancel_request();
```

### **AudioResponsePlayer**
```cpp
esp_err_t init(AudioPipelineManager* pipeline);
esp_err_t queue_audio_chunk(const uint8_t* audio_data, 
                          size_t length, 
                          bool is_final);
esp_err_t stop_playback();
```

---

## License & Support

This implementation builds upon your existing ESP32 Bluetooth speaker project and integrates seamlessly with the ESP-ADF framework. The voice assistant features require internet connectivity and a valid Gemini API key.

For support or questions about this implementation, refer to the code comments and ESP-IDF documentation. 