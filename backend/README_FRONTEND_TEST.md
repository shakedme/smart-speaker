# BT Speaker Frontend Test Interface

This frontend test interface mimics the behavior of the ESP32 WebSocket voice client to test the backend's LLM dialogue streaming functionality.

## Features

The frontend implements the same WebSocket protocol as the ESP32 device:

- **WebSocket Connection**: Connects to the backend at `ws://localhost:8080/voice`
- **Audio Recording**: Captures microphone input and converts to 16kHz mono PCM
- **Real-time Streaming**: Sends audio chunks as binary WebSocket frames
- **Control Messages**: Sends JSON control messages (`start_recording`, `stop_recording`, `cancel`)
- **Audio Response**: Receives streamed MP3 audio response from the backend
- **Status Monitoring**: Shows connection status, recording status, and data statistics

## How to Use

1. **Start the Backend Server**:
   ```bash
   cd backend
   npm run build
   npm start
   ```

2. **Open the Frontend**:
   - Navigate to `http://localhost:8080` in your web browser
   - The interface will load automatically

3. **Test the Voice Pipeline**:
   - Click **"Connect"** to establish WebSocket connection
   - Click **"Start Recording"** to begin voice capture
   - Speak into your microphone
   - Click **"Stop Recording"** to trigger the STT→LLM→TTS pipeline
   - Wait for the audio response and play it back

## Protocol Implementation

The frontend follows the exact same protocol as the ESP32 client:

### Control Messages (JSON Text Frames)
```json
{"type": "start_recording"}
{"type": "stop_recording"}
{"type": "cancel"}
```

### Audio Data (Binary Frames)
- Raw PCM audio data (16kHz, 16-bit, mono)
- Sent in real-time chunks during recording

### Server Responses
```json
{"type": "audio_start"}
{"type": "audio_end"}
{"type": "error", "message": "Error description"}
```

### Audio Response (Binary Frames)
- MP3 audio data streamed in chunks
- Combined and played back in the browser

## Browser Requirements

- **Modern Browser**: Chrome, Firefox, Safari, Edge (WebRTC support required)
- **HTTPS/Localhost**: Microphone access requires secure context
- **WebSocket Support**: All modern browsers support this

## Troubleshooting

1. **Microphone Permission**: Grant microphone access when prompted
2. **Audio Issues**: Check browser audio settings and microphone levels
3. **Connection Failed**: Ensure backend server is running on port 8080
4. **No Response**: Check that API keys are configured in `.env` file

## Development Notes

The frontend uses:
- **Web Audio API**: For real-time audio processing and PCM conversion
- **MediaRecorder API**: For microphone access
- **WebSocket API**: For bi-directional communication
- **Blob API**: For audio playback

This allows for testing the exact same voice processing pipeline that will be used on the ESP32 device. 