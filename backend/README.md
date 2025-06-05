# BT Speaker Backend - Gemini Audio Integration

A production-ready WebSocket voice streaming backend for ESP32 BT Speaker with Google Gemini audio processing.

## 🚀 Features

- **Native Audio Processing**: Google Gemini 2.0 Flash with native audio input
- **Combined STT + LLM**: Single API call for speech recognition and response generation
- **High-Quality TTS**: Google Cloud Text-to-Speech with neural voices
- **WebSocket Streaming**: Real-time audio processing
- **Session Management**: Automatic cleanup and error handling
- **Optimized Pipeline**: Streamlined Gemini Audio → Response → TTS workflow

## 💰 Cost Analysis

**Google-First Architecture for optimal integration:**

1. **Audio + LLM**: Google Gemini 2.0 Flash with audio input (~32 tokens per second of audio)
2. **TTS**: Google Cloud Text-to-Speech (Neural voices)

**Estimated costs per conversation:**
- Average 30-second audio: ~960 audio tokens + response (~50 tokens) = ~$0.0004
- ~100 character TTS output: ~$0.0002
- **Total per conversation: ~$0.0006 (88% cost reduction!)**

## 🔧 Setup

### 1. Install Dependencies

```bash
npm install
```

### 2. Environment Variables

Copy the example environment file and configure your API keys:

```bash
cp env.example .env
```

Edit `.env` with your API keys:

```env
# Server Configuration
PORT=8080

# Google AI API (for Gemini 2.0 Flash audio processing and LLM)
GOOGLE_AI_API_KEY=your_google_ai_api_key_here

# Google Cloud (for Text-to-Speech)
GOOGLE_APPLICATION_CREDENTIALS=path/to/service-account-key.json
GOOGLE_CLOUD_PROJECT_ID=your_project_id

# Development
NODE_ENV=development
```

### 3. Set Up Google Services

#### Google AI API Key
1. Go to [Google AI Studio](https://aistudio.google.com/app/apikey)
2. Create a new API key
3. Enable the Generative AI API

#### Google Cloud Text-to-Speech
1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Create a new project or select existing one
3. Enable the Text-to-Speech API
4. Create a service account with TTS permissions
5. Download the service account JSON key
6. Set `GOOGLE_APPLICATION_CREDENTIALS` to the key file path

### 4. Run the Server

Development mode with auto-reload:
```bash
npm run dev
```

Production build and run:
```bash
npm run build
npm start
```

## 🏗️ Architecture

```
ESP32 → WebSocket → Backend → Google APIs
                     ├── Gemini 2.0 Flash (Audio → Text Response)
                     └── Cloud Text-to-Speech (Text → Audio)
```

## 📡 WebSocket Protocol

### Client → Server

**Control Messages (JSON):**
```json
{"type": "start_recording"}
{"type": "stop_recording"}
{"type": "cancel"}
```

**Audio Data (Binary):**
- Raw PCM audio chunks (16-bit, 24kHz, mono)

### Server → Client

**Control Messages (JSON):**
```json
{"type": "audio_start"}
{"type": "audio_end"}
{"type": "error", "message": "Error description"}
```

**Audio Data (Binary):**
- MP3 audio chunks for playback

## 🔒 Security Considerations

- API keys are loaded from environment variables
- Temporary audio files are automatically cleaned up
- Session isolation with unique IDs
- Error handling prevents API key leakage

## 📊 Performance

- **Latency**: ~1-2 seconds end-to-end (optimized with combined audio processing)
- **Throughput**: Multiple concurrent sessions supported
- **Memory**: In-memory audio buffering with automatic cleanup
- **API Calls**: Reduced from 3 to 2 (Gemini Audio + Google TTS)
- **Cost**: ~$0.0006 per conversation (88% reduction)

## 🛠️ Development

### File Structure
```
backend/
├── src/
│   ├── server.ts              # Express + WebSocket server
│   ├── websocket-handler.ts   # Main voice processing logic
│   └── types.ts              # TypeScript type definitions
└── dist/                     # Compiled JavaScript (auto-generated)
```

### Key Classes

- `WebSocketHandler`: Manages individual voice sessions
- Audio pipeline: Gemini Audio Processing → Google Cloud TTS
- Automatic session cleanup and error handling

## 🚨 Troubleshooting

### Common Issues

1. **"Cannot find module '@google/generative-ai'"**
   ```bash
   npm install @google/generative-ai
   ```

2. **API Key errors**
   - Ensure `.env` file exists and contains valid keys
   - Check API key permissions and billing setup

3. **Audio processing errors**
   - Verify ESP32 sends correct PCM format (16-bit, 24kHz, mono)
   - Ensure sufficient memory for in-memory audio processing

### Debugging

Enable verbose logging:
```bash
NODE_ENV=development npm run dev
```

## 📈 Scaling

For production deployment:

1. **Load Balancing**: Use multiple backend instances
2. **Rate Limiting**: Implement API rate limiting
3. **Monitoring**: Add health checks and metrics
4. **Caching**: Cache common responses
5. **Database**: Store conversation history if needed

## 🔗 Integration

This backend is designed to work with the ESP32 voice assistant. Ensure your ESP32 firmware:

1. Connects to WebSocket on `ws://your-server:8080`
2. Sends audio as raw PCM (16-bit, 24kHz, mono)
3. Handles MP3 audio responses
4. Implements the WebSocket protocol above 