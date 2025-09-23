# BT Speaker

An ESP32-based smart speaker that combines classic Bluetooth audio, Spotify Connect streaming, and a cloud powered voice assistant. The firmware is built with ESP-IDF and ESP-ADF, while a Node.js backend provides speech-to-intent processing, text-to-speech responses, and Spotify search automation.

## Features
- ESP32 firmware with modular managers for Wi-Fi, audio sources, and voice assistant flows
- Bluetooth A2DP sink with AVRCP transport controls and automatic source switching
- Spotify Connect client powered by `cspot`, including Zeroconf pairing and persistent credentials stored in NVS
- WebSocket voice assistant that streams microphone audio to a backend for wake word detection, Gemini intent parsing, Google Cloud speech services, and real-time voice replies
- Shared audio pipeline that can queue voice responses over active music playback
- Optional wake word model training tools for the custom "Hey Rufus" hotword

## Repository Layout
- `main/` – ESP-IDF application entry point and firmware modules (audio pipelines, Wi-Fi, voice assistant, Spotify integration)
- `components/audio_board/` – Board definition for ESP32-DevKitC wired to a PCM5102A DAC and external amplifier
- `cspot/` – Embedded Spotify Connect client and dependencies used by the firmware
- `backend/` – TypeScript WebSocket server that handles wake word detection, Gemini requests, Google Cloud speech APIs, and Spotify search
- `scripts/*.sh` – Helper scripts for building or analysing firmware images

## Firmware (ESP-IDF + ESP-ADF)
### Prerequisites
- ESP-IDF v5.x installed and exported (`IDF_PATH` set)
- ESP-ADF cloned and available via `ADF_PATH`
- Toolchain for ESP32 with CMake/Ninja (installed through ESP-IDF tools)
- ESP32 module with PSRAM (DevKitC + ESP32-WROVER recommended)
- PCM5102A I2S DAC connected using the pins defined in `components/audio_board/include/board.h`
- I2S MEMS microphone (e.g. SPH0645) wired to pins configured in `main/include/voice_config.hpp`

### Build and Flash
```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
Replace the serial port with the one that matches your development board. A combined `flash monitor` step flashes the firmware and attaches the serial monitor.

### Configuration
- Update Wi-Fi credentials, WebSocket endpoint, and microphone pins in `main/include/voice_config.hpp`
- The device name advertised to Spotify can be tuned in `main/include/spotify_config.hpp`
- ESP-IDF menuconfig is still available for low-level tuning: `idf.py menuconfig`

### Runtime Behaviour
- The firmware boots into Bluetooth mode and automatically switches audio sources when Spotify authentication completes or when the voice assistant plays back responses
- Spotify pairing: connect the ESP32 to Wi-Fi, open the Spotify app, and select `ESP32_SPEAKER` from the device picker to complete Zeroconf authentication (credentials persist via NVS)
- Voice assistant: microphone audio is streamed over WebSocket to the backend; intents trigger Spotify actions or stream TTS audio back through the shared pipeline

## Voice Assistant Backend
The backend lives in `backend/` and powers intent recognition and TTS playback.

### Setup
```bash
cd backend
npm install
npm run build
```

### Required Environment Variables
Create a `.env` file in `backend/` (or export the variables) before starting the server:
- `PORT` – HTTP/WebSocket port (default 8080)
- `GOOGLE_AI_API_KEY` – API key for Google Gemini
- `PICOVOICE_ACCESS_KEY` – Access key for Picovoice Porcupine/Cobra SDKs
- `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` – Client credentials used for search & intent follow-up
- Optional: `GOOGLE_APPLICATION_CREDENTIALS` pointing to a service account JSON if Google Cloud Speech/TTS is configured to use service accounts

Place the Picovoice keyword file (`Hey-Rufus_en_mac_v3_0_0.ppn`) in the backend root (already referenced by relative path).

### Run the Server
```bash
npm run dev:live    # watches TypeScript and restarts on change
# or
npm run start:live  # build once and run the compiled server
```
The server hosts a minimal web UI at `http://localhost:PORT` for testing audio streaming, and exposes the WebSocket endpoint at `/voice` that the ESP32 firmware connects to.

## Wake Word Model
The `hey_rufus/` directory contains scripts to curate datasets, extract MFCC features, train TensorFlow Lite models, and export Picovoice-compatible wake word models. Refer to the individual Python scripts for usage; they require NumPy, TensorFlow, and the Picovoice SDK.

## Development Tips
- `build_optimized.sh` / `analyze_build.sh` wrap common ESP-IDF build and inspection commands
- `create_precompiled_libs.sh` can regenerate precompiled third-party libraries when updating ESP-IDF/ADF versions
- When testing voice assistant flows without hardware, run the backend locally and point `CONFIG_WEBSOCKET_SERVER_URL` to `ws://<host>:8080/voice`
- Use `idf.py monitor` logs to watch for audio source transitions, Spotify authentication status, and voice assistant state changes
