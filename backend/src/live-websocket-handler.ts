import { WebSocket } from 'ws';
import { v4 as uuidv4 } from 'uuid';
import { GoogleGenAI, Modality } from '@google/genai';

interface ControlMessage {
    type: 'start_session' | 'end_session' | 'audio_start' | 'audio_end' | 'error';
    message?: string;
}

export class LiveWebSocketHandler {
    private ws: WebSocket;
    private sessionId: string;
    private isSessionActive: boolean = false;
    private geminiSession: any = null;
    private gemini: GoogleGenAI;
    private responseQueue: any[] = [];

    constructor(ws: WebSocket) {
        this.ws = ws;
        this.sessionId = uuidv4();
        
        // Initialize Google Gemini client for Live API
        this.gemini = new GoogleGenAI({ apiKey: process.env.GOOGLE_AI_API_KEY || '' });

        console.log(`Live Session ${this.sessionId} created`);
    }

    public handleMessage(data: Buffer, isBinary: boolean): void {
        if (isBinary) {
            this.handleAudioData(data);
        } else {
            this.handleControlMessage(data);
        }
    }

    private handleControlMessage(data: Buffer): void {
        try {
            const message: ControlMessage = JSON.parse(data.toString());
            console.log(`Live Session ${this.sessionId}: Received control message:`, message.type);

            switch (message.type) {
                case 'start_session':
                    this.startLiveSession();
                    break;
                case 'end_session':
                    this.endLiveSession();
                    break;
                default:
                    console.warn(`Live Session ${this.sessionId}: Unknown control message:`, message.type);
            }
        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Failed to parse control message:`, error);
            this.sendError('Invalid control message format');
        }
    }

    private handleAudioData(data: Buffer): void {
        if (!this.isSessionActive || !this.geminiSession) {
            console.warn(`Live Session ${this.sessionId}: Received audio data while session not active`);
            return;
        }

        try {
            // Convert Buffer to base64 for Gemini Live API
            const base64Audio = data.toString('base64');
            
            // Send audio to Gemini Live API using correct format
            this.geminiSession.sendRealtimeInput({
                audio: {
                    data: base64Audio,
                    mimeType: "audio/pcm;rate=16000"
                }
            });

        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Error sending audio to Gemini:`, error);
        }
    }

    private async startLiveSession(): Promise<void> {
        if (this.isSessionActive) {
            console.warn(`Live Session ${this.sessionId}: Session already active`);
            return;
        }

        try {
            console.log(`Live Session ${this.sessionId}: Starting Gemini Live session`);

            // Create Live API connection using correct syntax
            this.geminiSession = await this.gemini.live.connect({
                model: 'gemini-2.0-flash-live-001',
                config: {
                    responseModalities: [Modality.AUDIO],
                    speechConfig: {
                        voiceConfig: { prebuiltVoiceConfig: { voiceName: "Aoede" } }
                    },
                    systemInstruction: `You are a helpful voice assistant for a smart Bluetooth speaker. 
                    Provide concise, natural responses suitable for voice output. 
                    Keep responses conversational and under 100 words.
                    You can help with general questions, check weather, tell jokes, and provide information.`
                },
                callbacks: {
                    onopen: () => {
                        console.log(`Live Session ${this.sessionId}: Gemini Live connection opened`);
                        this.isSessionActive = true;
                        this.sendControlMessage({ type: 'audio_start' });
                    },
                    onmessage: (message: any) => {
                        this.handleGeminiMessage(message);
                    },
                    onerror: (error: any) => {
                        console.error(`Live Session ${this.sessionId}: Gemini Live error:`, error);
                        this.sendError('Gemini Live connection error');
                    },
                    onclose: (event: any) => {
                        console.log(`Live Session ${this.sessionId}: Gemini Live connection closed:`, event.reason);
                        this.isSessionActive = false;
                        this.sendControlMessage({ type: 'audio_end' });
                    }
                }
            });

        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Failed to start Live session:`, error);
            this.sendError('Failed to start live session');
        }
    }

    private async endLiveSession(): Promise<void> {
        if (!this.isSessionActive) {
            console.warn(`Live Session ${this.sessionId}: No active session to end`);
            return;
        }

        try {
            console.log(`Live Session ${this.sessionId}: Ending Gemini Live session`);
            
            if (this.geminiSession) {
                this.geminiSession.close();
                this.geminiSession = null;
            }
            
            this.isSessionActive = false;
            this.sendControlMessage({ type: 'audio_end' });
            
        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Error ending session:`, error);
        }
    }

    private handleGeminiMessage(message: any): void {
        try {
            console.log(`Live Session ${this.sessionId}: Received Gemini message:`, message.type || 'unknown');
            
            // Log the full message structure to understand the format
            console.log(`Live Session ${this.sessionId}: Full message keys:`, Object.keys(message));

            // Handle direct audio data responses from Gemini (skip - we'll use serverContent instead)
            if (message.data) {
                console.log(`Live Session ${this.sessionId}: Skipping direct audio data (using serverContent instead): ${message.data.length} chars base64`);
            }

            // Handle server content with model turn (for audio responses)
            if (message.serverContent && message.serverContent.modelTurn) {
                console.log(`Live Session ${this.sessionId}: ServerContent modelTurn found`);
                const parts = message.serverContent.modelTurn.parts;
                console.log(`Live Session ${this.sessionId}: ModelTurn parts count: ${parts ? parts.length : 0}`);
                
                if (parts) {
                    for (let i = 0; i < parts.length; i++) {
                        const part = parts[i];
                        console.log(`Live Session ${this.sessionId}: Part ${i} keys:`, Object.keys(part));
                        
                        if (part.inlineData) {
                            console.log(`Live Session ${this.sessionId}: Part ${i} inlineData MIME: ${part.inlineData.mimeType}`);
                            console.log(`Live Session ${this.sessionId}: Part ${i} inlineData size: ${part.inlineData.data.length} chars`);
                            
                            if (part.inlineData.mimeType.includes('audio')) {
                                // Convert base64 audio to buffer and send to frontend
                                const audioBuffer = Buffer.from(part.inlineData.data, 'base64');
                                console.log(`Live Session ${this.sessionId}: Decoded part buffer: ${audioBuffer.length} bytes`);
                                this.sendControlMessage({ type: 'audio_start' });
                                this.ws.send(audioBuffer);
                                this.sendControlMessage({ type: 'audio_end' });
                            }
                        }
                    }
                }
            }

            // Handle setup complete
            if (message.setupComplete) {
                console.log(`Live Session ${this.sessionId}: Gemini Live setup complete`);
            }

            // Handle turn complete
            if (message.serverContent && message.serverContent.turnComplete) {
                console.log(`Live Session ${this.sessionId}: Gemini turn complete`);
            }

        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Error handling Gemini message:`, error);
        }
    }

    private sendControlMessage(message: ControlMessage): void {
        if (this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(message));
        }
    }

    private sendError(message: string): void {
        console.error(`Live Session ${this.sessionId}: Error: ${message}`);
        this.sendControlMessage({ type: 'error', message });
    }

    public async cleanup(): Promise<void> {
        try {
            await this.endLiveSession();
        } catch (error) {
            console.error(`Live Session ${this.sessionId}: Error during cleanup:`, error);
        }
        
        console.log(`Live Session ${this.sessionId}: Cleaned up`);
    }
} 