# 🎤 Gemini Live API Setup Guide

## ✅ **Implementation Complete!**

The WebSocket handler has been successfully updated to use Google's **Gemini Live API** for real-time voice conversations.

## 🚀 **What's New**

### **Before vs After:**
- **Before**: Audio → OpenAI Whisper (STT) → Gemini (LLM) → Google Cloud TTS → Response
- **After**: Audio → **Gemini Live API** (Real-time STT + LLM + Response) → Frontend

### **Key Improvements:**
1. **⚡ Lower Latency**: Real-time streaming instead of batch processing
2. **💰 Cost Reduction**: Single API call instead of 3 separate calls
3. **🎯 Better UX**: Continuous conversation flow with interruption support
4. **🔄 Bidirectional**: True real-time dialogue experience

## 🎯 **How to Test**

### **1. Environment Setup**
Create a `.env` file in the `backend/` directory:
```env
PORT=8080
GOOGLE_AI_API_KEY=your_api_key_here
NODE_ENV=development
```

### **2. Start the Live Server**
```bash
cd backend
npm run dev:live
```

### **3. Open the Frontend**
Navigate to: `http://localhost:8080`

### **4. Test the Live API**
1. Click **"Connect"** to establish WebSocket connection
2. Click **"Start Live Session"** to begin voice chat
3. **Speak** - your voice is streamed in real-time to Gemini
4. **Listen** - Gemini responds with text (audio coming soon)
5. Click **"End Session"** when done

## 🛠 **Technical Details**

### **Live API Features Used:**
- **Model**: `gemini-2.0-flash-live-001`
- **Response Mode**: `TEXT` (configurable to `AUDIO`)
- **Audio Format**: 16-bit PCM, 16kHz, mono
- **Real-time Streaming**: Bidirectional WebSocket communication

### **Frontend Changes:**
- Updated button text: "Start Live Session" / "End Session"
- Control messages: `start_session` / `end_session`
- Real-time audio streaming to Gemini

### **Backend Changes:**
- New `LiveWebSocketHandler` using `@google/genai` package
- Real-time audio processing with `sendRealtimeInput()`
- Session management with proper cleanup
- Error handling for connection issues

## 🎵 **Audio Response (Future)**

To enable audio responses instead of text:

```typescript
// In live-websocket-handler.ts, line ~74
config: {
    responseModalities: [Modality.AUDIO], // Change from TEXT to AUDIO
    speechConfig: {
        voiceConfig: { prebuiltVoiceConfig: { voiceName: "Aoede" } }
    }
}
```

## 🔧 **Troubleshooting**

### **Common Issues:**
1. **"fetch is not defined"**: Make sure you're using Node.js 18+
2. **WebSocket connection fails**: Check your API key and internet connection
3. **No audio detected**: Ensure microphone permissions are granted
4. **Session timeout**: Gemini Live sessions have time limits without session resumption

### **Debug Mode:**
Check browser DevTools Console and backend terminal for detailed logs.

## 📚 **Resources**
- [Gemini Live API Documentation](https://ai.google.dev/gemini-api/docs/live)
- [Google AI Studio Live Demo](https://aistudio.google.com/)
- [Live API Examples](https://github.com/google-gemini/cookbook)

## 🎉 **Ready for ESP32!**

Once you've tested the Live API with the web frontend, you can proceed to integrate this with your ESP32 hardware for the complete smart speaker experience. 