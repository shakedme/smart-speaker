# BT Speaker

An ESP32-based smart speaker that combines Bluetooth (A2DP) audio, **Spotify Connect** (embedded **cspot** client), and a **WebSocket voice assistant**. Firmware uses **ESP-IDF** and **ESP-ADF**. The **`backend/`** service is a **NestJS** app: it receives uplink audio from the device, runs **wake word** (LiveKit ONNX worker), **VAD** (Silero via `avr-vad`), **Gemini** intent classification on speech, **Gemini Live** for spoken answers, and optional **Spotify** / **calendar** tools—then streams PCM and JSON commands back to the speaker.

For a fuller architecture picture (uplink framing, AEC, module map), see [`.cursor/rules/project_architecture.mdc`](.cursor/rules/project_architecture.mdc).

## Features

- Modular firmware: `AppManager` coordinates Wi‑Fi, `AudioSourceManager` (Bluetooth / Spotify / LLM voice playback), and `VoiceAssistantManager` (mic + WebSocket client).
- **Bluetooth** A2DP sink with AVRCP-style transport control and source switching when links or Spotify state change.
- **Spotify Connect** via **cspot** (Zeroconf discovery, credentials in NVS). Advertised Connect name defaults to **`Rufus`** (`main/include/spotify_config.hpp`).
- **Voice assistant**: 16 kHz mono uplink (PCM, IMA‑ADPCM, or framed mic + optional playback reference for backend **WebRTC AEC3**). Downlink assistant audio at **24 kHz** into the shared pipeline.
- **Backend intents**: Spotify play requests, playback controls to the device, natural-language Q&A via Gemini Live, and **calendar** tools (Google / Microsoft / ICS / mock—configured on the server only).
- Static test UI: `backend/public/` is served at the HTTP root; WebSocket voice is at **`/voice`**. **`GET /health`** reports basic status and whether the wakeword worker is up.

## Repository layout

| Path | Purpose |
|------|--------|
| `backend/` | NestJS server: `/voice` WebSocket sessions, Gemini, tools, optional `public/` UI. |
| `shared_firmware/` | Shared ESP-IDF components used by both nodes (protocol, Wi‑Fi, WebSocket client, uplink framing/ADPCM). |
| `shared_firmware/components/speaker_protocol/` | Node identity, roles, board profiles, capabilities, UI message names. |
| `shared_firmware/components/smart_common/` | Wi‑Fi manager, WebSocket client, uplink framing, `voice_config.hpp`. |
| `playback_firmware/` | Self-contained ESP32/WROVER **playback node**: Spotify/cspot, Bluetooth, speaker output, LLM downlink, playback reference. |
| `playback_firmware/cspot/` | Vendored Spotify Connect stack (playback node only). |
| `playback_firmware/components/` | Playback-only board and Bluetooth components (`audio_board`, `bluetooth_service`). |
| `voice_firmware/` | Self-contained ESP32-S3 **voice/UI node**: mic uplink, LCD UI, now-playing and waveform rendering. |
| `main/` | **Legacy** monolithic single-MCU firmware (scheduled for removal; do not add new code here). |
| `scripts/*.sh` | Firmware build / analysis helpers. |

## Firmware (ESP-IDF + ESP-ADF)

### Prerequisites

- **ESP-IDF** v5.x (`IDF_PATH`, `idf.py` on `PATH`), bundled with your ESP-ADF install.
- **ESP-ADF**: set `ADF_PATH` to your latest ADF checkout and **`source $ADF_PATH/export.sh`** before building. Both `playback_firmware` and `voice_firmware` share this environment by default.
- Optional: set `VOICE_ADF_PATH` only if the voice node must build against a different ADF install than playback.
- Toolchain for ESP32 / ESP32-S3 (CMake/Ninja via ESP-IDF tools installer).
- **PSRAM** recommended (e.g. ESP32-WROVER + DevKitC-class board).
- **PCM5102A** on I2S per `components/audio_board/include/board.h` (and project pin docs).
- **MEMS mics** (e.g. SPH0645) per `main/include/voice_config.hpp`.

### Build and flash

```bash
source $ADF_PATH/export.sh   # required for ADF-based builds
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Use the serial port that matches your machine (`COM*`, `/dev/tty.usbserial-*`, etc.).

### Two-node firmware builds

The two-MCU split uses independent ESP-IDF project roots but **one shared ADF/IDF environment**. Source it once, then build each target:

```bash
source $ADF_PATH/export.sh

./playback_firmware/build.sh set-target esp32
./playback_firmware/build.sh build

./voice_firmware/build.sh set-target esp32s3
./voice_firmware/build.sh build
```

Each project's `build.sh` auto-sources `$ADF_PATH/export.sh` when `idf.py` is not already on `PATH`. The voice project falls back to `VOICE_ADF_PATH` only when that variable is set.

Or equivalently:

```bash
source $ADF_PATH/export.sh
idf.py -C playback_firmware set-target esp32 build
idf.py -C voice_firmware set-target esp32s3 build
```

Before flashing or monitoring, enumerate and identify ports instead of assuming names:

```bash
ls -1 /dev/cu.* /dev/tty.* 2>/dev/null
esptool.py --port <candidate-port> chip_id
```

Map one detected port to the ESP32/WROVER playback node and one to the ESP32-S3-Korvo-2 voice/UI node before running any `idf.py -p ... flash monitor` command.

### Protocol v2

Both nodes connect to backend `/voice` and first send a JSON `hello` message with `protocol_version`, `speaker_id`, `node_id`, `node_role`, `board_profile`, and `capabilities`.

- `playback_node` receives LLM PCM, Spotify commands, and playback controls; it sends playback reference frames and playback/metadata state.
- `voice_ui_node` sends mic frames and receives `ui.now_playing`, `ui.waveform`, and session state messages.
- Mic frames and playback reference frames keep the existing framed binary header semantics used by `shared_firmware/components/smart_common/include/uplink_audio_frame.hpp` and `backend/src/audio/uplink-frame.ts`.

### Configuration

- **Wi‑Fi**, **WebSocket URL** (`CONFIG_WEBSOCKET_SERVER_URL`), and mic-related settings: `shared_firmware/components/smart_common/include/voice_config.hpp`.
- **Spotify Connect** name/port: `playback_firmware/main/include/spotify_config.hpp`.
- Low-level SDK options: `idf.py menuconfig`.

### Runtime behaviour

- Typical boot: Bluetooth available; with Wi‑Fi and Spotify auth, **Spotify** can become the active source; **voice responses** preempt playback then prior behaviour is restored as implemented in `AudioSourceManager`.
- **Spotify pairing**: on the same LAN as the speaker, pick the Connect device (default name **Rufus**) in the Spotify app and complete Zeroconf login; tokens persist in NVS.
- **Voice**: firmware streams uplink to the backend; backend replies with JSON (state, commands) and binary PCM for `LLMPipeline`.

## Voice assistant backend (`backend/`)

NestJS application: HTTP + **`ws`** on **`/voice`** (see `VoiceGateway`). Uses **`@google/genai`** (Gemini 2.x for intent from WAV; Live API for streaming voice).

### Setup

```bash
cd backend
npm install
npm run build
```

`npm run build` also copies the wakeword worker script into `dist/` (see `package.json`).

### Environment variables

Copy **`backend/.env.example`** to **`backend/.env`** and fill in at least:

| Variable | Purpose |
|----------|--------|
| `PORT` | HTTP + WebSocket listen port (default `8080`). |
| `GOOGLE_AI_API_KEY` | Google AI / Gemini API access. |
| `SPOTIFY_CLIENT_ID` / `SPOTIFY_CLIENT_SECRET` | Server-side search / URI resolution for “play …” intents. |
| `WAKEWORD_MODEL_PATH` | ONNX model path (e.g. trained **Okay Rufus** model). |
| `WAKEWORD_PYTHON` | Python interpreter that has **`livekit-wakeword`** installed (default `.venv/bin/python`). |

Common optional settings (see `.env.example` for the full list):

- **`USER_TIMEZONE`**, **`CALENDAR_PROVIDER`** (`none` \| `mock` \| `google` \| `microsoft`) and provider-specific tokens or ICS URLs.
- **Silero VAD**: `VAD_THRESHOLD`, `VAD_SILENCE_TIMEOUT_MS`.
- **Wake word tuning**: `WAKEWORD_THRESHOLD`, `WAKEWORD_WINDOW_MS`, `WAKEWORD_DEBOUNCE_MS`, `WAKEWORD_INFERENCE_INTERVAL_MS`.
- **Backend AEC3**: `BACKEND_AEC3_ENABLED`, `AEC_INITIAL_DELAY_MS` (requires framed uplink with playback reference from firmware).
- **Diagnostics / captures**: `VOICE_DEBUG`, `WAKEWORD_CAPTURE_AUDIO`, etc.

Calendar web UI for pasting a secret iCal URL is mentioned in `.env.example` (`http://localhost:8080/calendar/settings` when running locally on default port).

### Run

```bash
npm run dev         # nest start --watch
# or
npm run start:live  # build then node dist/main.js
```

- **WebSocket**: `ws://<host>:<PORT>/voice` (must match `CONFIG_WEBSOCKET_SERVER_URL` on the device).
- **Browser**: `http://localhost:<PORT>` serves `public/index.html` for quick tests.

Install the Python side for wake word inference, for example:

```bash
python3.11 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pip install livekit-wakeword
```

## Wake word model (Okay Rufus)

Production hotword uses **[livekit-wakeword](https://github.com/livekit/livekit-wakeword)**. Train/export ONNX, point `WAKEWORD_MODEL_PATH` at the `.onnx` file; threshold can be taken from eval JSON (see `backend/models/wakeword/README.md`).

```bash
cd backend
python3.11 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pip install -r models/wakeword/requirements-train.txt
npm run train:wakeword:prod   # long-running; see models/wakeword/README.md
npm run start:live
```

## Development tips

- `build_optimized.sh` / `analyze_build.sh` — common ESP-IDF build and size analysis.
- `create_precompiled_libs.sh` — refresh precompiled third-party libs when upgrading IDF/ADF.
- Hardware-free backend tests: run the server, set `CONFIG_WEBSOCKET_SERVER_URL` to `ws://<your-host>:<PORT>/voice`.
- **`idf.py monitor`** — watch audio source changes, Spotify auth, and voice assistant logs.
