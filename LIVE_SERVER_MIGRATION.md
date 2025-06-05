# Live Server Migration for ESP32 Firmware

## Overview
The ESP32 firmware has been updated to work with the live server (`live-server.ts`) instead of the regular server. This change enables real-time bidirectional voice communication using Google's Gemini Live API.

## Changes Made

### 1. WebSocket Protocol Updates
Updated the WebSocket voice client to use the live server protocol:

**Before (Recording Protocol):**
- `{"type":"start_recording"}` - Start recording session  
- `{"type":"stop_recording"}` - Stop recording session
- `{"type":"cancel"}` - Cancel operation

**After (Live Session Protocol):**
- `{"type":"start_session"}` - Start live session
- `{"type":"end_session"}` - End live session
- Audio data is sent as binary frames during the session

### 2. Files Modified

#### `main/src/websocket_voice_client.cpp`
- Updated `start_recording_session()` to send `start_session` message
- Updated `stop_recording_session()` to send `end_session` message  
- Updated `cancel_operations()` to send `end_session` instead of `cancel`
- Updated logging messages to reflect "live session" terminology

#### `main/include/websocket_voice_client.hpp`
- Added comments to clarify that methods now handle live sessions
- Updated state tracking comments

#### `main/include/voice_config.hpp`
- Updated comments from "Recording Configuration" to "Live Session Configuration"
- No URL changes needed - already pointing to correct `/voice` endpoint

### 3. Protocol Compatibility

The live server protocol is largely compatible with the existing ESP32 implementation:

**Control Messages (Text):**
- `audio_start` - Server begins sending audio response (unchanged)
- `audio_end` - Server finishes sending audio response (unchanged)  
- `error` - Server error messages (unchanged)

**Audio Data (Binary):**
- Binary frames containing PCM audio data (unchanged)
- 16kHz, 16-bit, mono for input (unchanged)
- 24kHz, 16-bit for output (unchanged)

### 4. Live Server Benefits

The live server provides several advantages:
- **Real-time bidirectional communication** via Google Gemini Live API
- **Lower latency** - no need for separate request/response cycles
- **Natural conversation flow** - can interrupt and respond dynamically
- **Improved audio quality** - optimized audio streaming pipeline

### 5. Network Configuration

The WebSocket URL remains unchanged:
```cpp
#define CONFIG_WEBSOCKET_SERVER_URL "ws://localhost:8080/voice"
```

This connects to the live server running on port 8080 with the `/voice` WebSocket endpoint.

### 6. Usage

The ESP32 firmware usage remains the same:
1. Wake word detection triggers voice command processing
2. ESP32 starts live session with `start_session` message
3. Audio data is streamed as binary frames during the session
4. Server responds with real-time audio via the same WebSocket connection
5. Session ends with `end_session` message

### 7. Testing

To test the updated firmware:
1. Start the live server: `npm run live`
2. Flash the updated firmware to ESP32
3. Trigger voice commands (wake word or manual trigger)
4. Verify real-time audio response playback

## Migration Complete

The ESP32 firmware is now fully compatible with the live server implementation and ready for real-time voice interaction using Google's Gemini Live API. 