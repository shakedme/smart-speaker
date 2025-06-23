import {WebSocket} from 'ws';
import {v4 as uuidv4} from 'uuid';
import {GoogleGenAI, Modality, Part} from '@google/genai';
import {Cobra} from "@picovoice/cobra-node";
import {Porcupine} from "@picovoice/porcupine-node";
import axios from 'axios';
import * as fs from 'fs';

// The state of the WebSocket session handler
type SessionState =
    'IDLE'
    | 'LISTENING'
    | 'PROCESSING_INTENT'
    | 'STREAMING_RESPONSE'
    | 'HANDLING_COMMAND'
    | 'WAITING_FOR_USER';

// Type for control messages (now mainly for errors)
interface ControlMessage {
    type: 'error';
    message?: string;
}

// VAD constants
const VAD_SENSITIVITY = 0.4; // 0.0 (lowest) to 1.0 (highest)
const VAD_SILENCE_TIMEOUT_MS = 1200; // Time of silence before processing
const CONVERSATION_TIMEOUT_MS = 5000; // Time to keep conversation session alive with no user input

// Audio constants
const INPUT_SAMPLE_RATE = 16000; // ESP32 now sends at 16kHz directly
const TARGET_SAMPLE_RATE = 16000; // VAD/Wake word models expect 16kHz
const USE_RESAMPLING = false; // Disabled - ESP32 sends 16kHz directly

export class LiveWebSocketHandler {
    private ws: WebSocket;
    private sessionId: string;
    private gemini: GoogleGenAI;
    private cobra: Cobra;
    private cobraFrameLength: number;
    private porcupine: Porcupine;
    private porcupineFrameLength: number;

    // State management
    private sessionState: SessionState = 'IDLE';
    private responseSession: any = null;

    // Buffers
    private audioBuffer: Buffer[] = []; // Stores the complete user utterance
    private vadBuffer: Buffer = Buffer.alloc(0); // Temporary buffer for VAD frame processing
    private porcupineBuffer: Buffer = Buffer.alloc(0); // Temporary buffer for wake word processing

    // VAD state
    private silenceTimer: NodeJS.Timeout | null = null;
    private wakeWordDetected: boolean = false;

    // Conversation management
    private conversationTimer: NodeJS.Timeout | null = null;
    private isGeminiSpeaking: boolean = false;

    // Debug audio recording
    private originalAudioBuffer: Buffer[] = []; // 44.1kHz original audio
    private resampledAudioBuffer: Buffer[] = []; // 16kHz resampled audio
    private recordingStartTime: number = 0;
    private readonly RECORDING_DURATION_MS = 10000; // 10 seconds
    private hasWrittenOriginalWav: boolean = false;
    private hasWrittenResampledWav: boolean = false;

    constructor(ws: WebSocket) {
        this.ws = ws;
        this.sessionId = uuidv4();

        // Initialize Google Gemini
        const googleApiKey = process.env.GOOGLE_AI_API_KEY;
        if (!googleApiKey) {
            throw new Error('GOOGLE_AI_API_KEY is not set in environment variables.');
        }
        this.gemini = new GoogleGenAI({apiKey: googleApiKey});

        // Initialize Picovoice Cobra VAD
        const picoAccessKey = process.env.PICOVOICE_ACCESS_KEY;
        if (!picoAccessKey) {
            throw new Error('PICOVOICE_ACCESS_KEY is not set in environment variables. Get one from Picovoice Console.');
        }
        this.cobra = new Cobra(picoAccessKey);
        this.cobraFrameLength = this.cobra.frameLength;

        // Initialize Porcupine Wake Word Detection
        this.porcupine = new Porcupine(picoAccessKey, ['./Hey-Rufus_en_mac_v3_0_0.ppn'], [0.5]);
        this.porcupineFrameLength = this.porcupine.frameLength;

        this.broadcastStateChange('LISTENING');
        this.recordingStartTime = Date.now();
        this.log('info', `Session created. State: ${this.sessionState}. Waiting for audio...`);
    }

    public handleMessage(data: Buffer, isBinary: boolean): void {
        if (!isBinary) {
            this.log('warn', `Received unexpected text message: ${data.toString()}`);
            return;
        }

        if (this.sessionState === 'LISTENING' || this.sessionState === 'WAITING_FOR_USER') {
            this.handleAudioData(data);
        } else {
            this.log('warn', `Ignoring audio data - not in listening state (current: ${this.sessionState})`);
        }
    }

    private handleAudioData(data: Buffer): void {
        if (this.sessionState !== 'LISTENING' && this.sessionState !== 'WAITING_FOR_USER') return;

        // If we were waiting for user input, transition back to listening
        if (this.sessionState === 'WAITING_FOR_USER') {
            this.broadcastStateChange('LISTENING');
            this.log('info', 'User started speaking again. Continuing conversation.');
        }

        // Add incoming audio to the main buffer for the full utterance
        this.audioBuffer.push(data);
        // Add processed audio to processing buffers
        this.vadBuffer = Buffer.concat([this.vadBuffer, data]);
        this.porcupineBuffer = Buffer.concat([this.porcupineBuffer, data]);

        // Process audio in VAD-compatible frames
        const vadFrameSizeInBytes = this.cobraFrameLength * 2; // 16-bit PCM
        const porcupineFrameSizeInBytes = this.porcupineFrameLength * 2; // 16-bit PCM

        while (this.vadBuffer.length >= vadFrameSizeInBytes) {
            const audioFrameBytes = this.vadBuffer.subarray(0, vadFrameSizeInBytes);
            this.vadBuffer = this.vadBuffer.subarray(vadFrameSizeInBytes);

            // Explicitly read Int16 values to ensure correct format for Cobra.
            const audioFrameInt16 = new Int16Array(this.cobraFrameLength);
            for (let i = 0; i < this.cobraFrameLength; i++) {
                audioFrameInt16[i] = audioFrameBytes.readInt16LE(i * 2);
            }

            const voiceProbability = this.cobra.process(audioFrameInt16);

            if (voiceProbability > VAD_SENSITIVITY) {
                this.resetConversationTimer();

                // Only check for wake word when VAD detects voice
                if (!this.wakeWordDetected) {
                    // Check same audio frame for wake word detection
                    if (this.porcupineBuffer.length >= porcupineFrameSizeInBytes) {
                        const porcupineFrameBytes = this.porcupineBuffer.subarray(0, porcupineFrameSizeInBytes);
                        this.porcupineBuffer = this.porcupineBuffer.subarray(porcupineFrameSizeInBytes);

                        const porcupineAudioFrameInt16 = new Int16Array(this.porcupineFrameLength);
                        for (let i = 0; i < this.porcupineFrameLength; i++) {
                            porcupineAudioFrameInt16[i] = porcupineFrameBytes.readInt16LE(i * 2);
                        }

                        const keywordIndex = this.porcupine.process(porcupineAudioFrameInt16);
                        if (keywordIndex !== -1) {
                            this.log('info', `Wake word detected! Starting silence timer.`);
                            this.wakeWordDetected = true;
                        }
                    }
                }

                if (this.wakeWordDetected) {
                    this.resetSilenceTimer();
                    break;
                }
            }
        }
    }

    private resetSilenceTimer(): void {
        if (this.silenceTimer) {
            clearTimeout(this.silenceTimer);
        }
        // If we don't hear more speech for the timeout duration, process the command only if wake word was detected
        this.silenceTimer = setTimeout(() => {
            if (this.wakeWordDetected) {
                this.log('info', `Silence detected after wake word. Processing accumulated audio.`);
                this.processIntent();
            } else {
                this.log('debug', `Silence detected but no wake word detected. Continuing to listen.`);
            }
        }, VAD_SILENCE_TIMEOUT_MS);
    }

    private resetConversationTimer(): void {
        if (this.conversationTimer) {
            clearTimeout(this.conversationTimer);
        }
        if (this.responseSession) {
            this.conversationTimer = setTimeout(() => {
                if (!this.isGeminiSpeaking) {
                    this.log('info', 'Conversation timeout. Closing session.');
                    this.endConversation();
                } else {
                    this.log('info', 'Conversation timeout delayed - Gemini is still speaking.');
                    // Reset timer to check again later
                    this.resetConversationTimer();
                }
            }, CONVERSATION_TIMEOUT_MS);
        }

    }

    private broadcastStateChange(newState: SessionState): void {
        this.sessionState = newState;
        if (this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({
                type: 'state_change',
                data: newState
            }));
        }
        this.log('info', `State changed to: ${newState}`);
    }

    private async processIntent(): Promise<void> {
        if ((this.sessionState !== 'LISTENING' && this.sessionState !== 'WAITING_FOR_USER') || this.audioBuffer.length === 0) {
            this.log('warn', 'processIntent called with no audio or in wrong state.');
            this.clearAudioBuffers();
            return;
        }

        this.broadcastStateChange('PROCESSING_INTENT');
        if (this.silenceTimer) clearTimeout(this.silenceTimer);

        this.log('info', `Sending ${this.audioBuffer.length} audio chunks to Gemini for intent detection.`);

        try {
            const fullAudio = Buffer.concat(this.audioBuffer);
            const wavBuffer = this.createWavBuffer(fullAudio);

            // If we already have a conversation session, continue it
            if (this.responseSession) {
                this.log('info', 'Continuing existing conversation session.');
                this.broadcastStateChange('STREAMING_RESPONSE');
                this.responseSession.sendClientContent({
                    turns: [{
                        parts: [{
                            inlineData: {
                                mimeType: 'audio/wav',
                                data: wavBuffer.toString('base64')
                            }
                        }]
                    }]
                });
                this.clearAudioBuffers();
                return;
            }

            // First interaction - determine intent
            const systemInstruction = `You are a smart home assistant intent detection system. 
            Your task is to analyze the user's speech and determine their intent.
            Respond with a single, minified JSON object with the format: {"intent": "...", "data": "..."}.
            Do not include any other text, markdown, or explanation.
            Possible intents are "spotify", "playback_control", and "information".
            - For "spotify", the "data" should be the song, artist, or playlist name.
            - For "playback_control", the "data" should be the action (e.g., "pause", "next", "volume up").
            - For "information", the "data" should be the user's full question.`;
            const contents: Part[] = [
                {text: systemInstruction},
                {
                    inlineData: {
                        mimeType: 'audio/wav',
                        data: wavBuffer.toString('base64')
                    }
                }
            ];
            const response = await this.gemini.models.generateContent({
                model: 'gemini-2.0-flash',
                contents
            });

            const text = response.text;
            if (!text) {
                this.log('error', 'Gemini response is empty.');
                this.sendError('Gemini response is empty.');
                this.resetToListening();
                return;
            }

            this.log('info', `Gemini intent response: ${text}`);
            const intentData = JSON.parse(text);

            await this.handleIntent(intentData);

        } catch (error) {
            this.log('error', `Failed to get intent from Gemini: ${error}`);
            this.sendError(`Could not determine intent: ${error}`);
            this.resetToListening();
        }
    }

    private createWavBuffer(pcmData: Buffer): Buffer {
        const sampleRate = 16000;
        const numChannels = 1;
        const bitsPerSample = 16;
        const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        const blockAlign = numChannels * (bitsPerSample / 8);
        const dataSize = pcmData.length;
        const fileSize = 36 + dataSize;

        const buffer = Buffer.alloc(44);

        // RIFF header
        buffer.write('RIFF', 0);
        buffer.writeUInt32LE(fileSize, 4);
        buffer.write('WAVE', 8);

        // "fmt " sub-chunk
        buffer.write('fmt ', 12);
        buffer.writeUInt32LE(16, 16); // Sub-chunk size (16 for PCM)
        buffer.writeUInt16LE(1, 20); // Audio format (1 for PCM)
        buffer.writeUInt16LE(numChannels, 22);
        buffer.writeUInt32LE(sampleRate, 24);
        buffer.writeUInt32LE(byteRate, 28);
        buffer.writeUInt16LE(blockAlign, 32);
        buffer.writeUInt16LE(bitsPerSample, 34);

        // "data" sub-chunk
        buffer.write('data', 36);
        buffer.writeUInt32LE(dataSize, 40);

        return Buffer.concat([buffer, pcmData]);
    }

    private async handleIntent(intent: { intent: string; data: any }): Promise<void> {
        switch (intent.intent) {
            case 'information':
                this.log('info', 'Intent is "information". Starting audio response session.');
                this.broadcastStateChange('STREAMING_RESPONSE');
                await this.startAudioResponseSession(intent.data);
                break;
            case 'spotify':
                this.log('info', `Intent is "spotify". Data: ${intent.data}`);
                this.broadcastStateChange('HANDLING_COMMAND');
                const uri = await this.resolveSpotifyUri(intent.data);
                if (uri) {
                    this.ws.send(JSON.stringify({
                        type: 'intent',
                        intent: 'spotify',
                        data: uri
                    }));
                } else {
                    this.sendError(`Could not find Spotify URI for: ${intent.data}`);
                }
                this.resetToListening(); // Command sent, go back to listening
                break;
            case 'playback_control':
                this.log('info', `Intent is "playback_control". Data: ${intent.data}`);
                this.broadcastStateChange('HANDLING_COMMAND');
                this.ws.send(JSON.stringify(({
                    type: 'intent',
                    intent: 'playback_control',
                    data: intent.data
                })));
                this.resetToListening(); // Command sent, go back to listening
                break;
            default:
                this.log('warn', `Unknown intent received: ${intent.intent}`);
                this.resetToListening();
                break;
        }
    }

    private async startAudioResponseSession(question: string): Promise<void> {
        try {
            this.responseSession = await this.gemini.live.connect({
                model: 'gemini-2.0-flash-live-001',
                config: {
                    responseModalities: [Modality.AUDIO],
                    speechConfig: {voiceConfig: {prebuiltVoiceConfig: {voiceName: "Aoede"}}},
                    systemInstruction: `You are a helpful voice assistant. Provide concise, natural responses. Keep your responses conversational and be ready for follow-up questions.`
                },
                callbacks: {
                    onopen: () => {
                        this.log('info', 'Audio response session opened.');
                    },
                    onmessage: (message: any) => this.handleAudioResponse(message),
                    onerror: (e: any) => {
                        this.log('error', `Audio response error: ${e}`);
                        this.endConversation();
                    },
                    onclose: () => {
                        this.log('info', 'Audio response session closed.');
                        // Don't reset state here - let conversation timeout handle it
                    },
                }
            });

            this.responseSession.sendClientContent({
                turns: question
            });

            // Clear audio buffers after sending
            this.clearAudioBuffers();

        } catch (error) {
            this.log('error', `Failed to start audio response session: ${error}`);
            this.resetToListening();
        }
    }

    private handleAudioResponse(message: any): void {
        // Handle turn completion
        if (message.serverContent?.turnComplete) {
            this.isGeminiSpeaking = false;
            this.log('info', 'Gemini finished speaking. Waiting for user response.');
            this.transitionToWaitingForUser();
            return;
        }

        // Handle audio data
        const audioData = message.serverContent?.modelTurn?.parts?.[0]?.inlineData?.data;
        if (audioData) {
            if (!this.isGeminiSpeaking) {
                this.isGeminiSpeaking = true;
                this.log('info', 'Gemini started speaking.');
            }
            const audioBuffer = Buffer.from(audioData, 'base64');
            // Stream audio back to the client
            if (this.ws.readyState === WebSocket.OPEN) {
                this.log('debug', `Sending audio response chunk of ${audioBuffer.length} bytes.`);
                this.ws.send(audioBuffer);
            }
        }
    }

    private transitionToWaitingForUser(): void {
        this.broadcastStateChange('WAITING_FOR_USER');
        this.resetConversationTimer(); // Start conversation timeout
        this.log('info', 'Transitioned to waiting for user. Conversation session remains open.');
    }

    private endConversation(): void {
        this.log('info', 'Ending conversation session.');
        if (this.responseSession) {
            this.responseSession.close();
            this.responseSession = null;
        }
        this.resetToListening();
    }

    private resetToListening(): void {
        this.clearAllTimers();
        this.clearAudioBuffers();
        this.broadcastStateChange('LISTENING');
        this.isGeminiSpeaking = false;
        this.log('info', 'Reset to listening state. Ready for next command.');
    }

    private clearAudioBuffers(): void {
        this.audioBuffer = [];
        this.vadBuffer = Buffer.alloc(0);
        this.porcupineBuffer = Buffer.alloc(0);
        this.wakeWordDetected = false;
    }

    private clearAllTimers(): void {
        if (this.silenceTimer) {
            clearTimeout(this.silenceTimer);
            this.silenceTimer = null;
        }
        if (this.conversationTimer) {
            clearTimeout(this.conversationTimer);
            this.conversationTimer = null;
        }
    }

    private sendError(message: string): void {
        console.error(`Session ${this.sessionId}: Error: ${message}`);
        if (this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({type: 'error', message} as ControlMessage));
        }
    }

    public async cleanup(): Promise<void> {
        this.log('info', 'Cleaning up session...');
        this.endConversation();
        this.clearAllTimers();
        this.sessionState = 'IDLE';
        this.cobra.release(); // Release Cobra resources
        this.porcupine.release(); // Release Porcupine resources
    }

    private log(level: 'info' | 'warn' | 'error' | 'debug', message: string): void {
        console.log(`[${level.toUpperCase()}] Session ${this.sessionId}: ${message}`);
    }

    // --- Spotify Integration ---
    private async resolveSpotifyUri(query: string): Promise<string | null> {
        if (!query) return null;
        const token = await this.getSpotifyAccessToken();
        if (!token) return null;
        const type = 'track,playlist,artist';
        const url = `https://api.spotify.com/v1/search?q=${encodeURIComponent(query)}&type=${type}&limit=1`;
        try {
            const resp = await axios.get(url, {headers: {Authorization: `Bearer ${token}`}});
            if (resp.data.tracks?.items.length > 0) return resp.data.tracks.items[0].uri;
            if (resp.data.playlists?.items.length > 0) return resp.data.playlists.items[0].uri;
            if (resp.data.artists?.items.length > 0) return resp.data.artists.items[0].uri;
            return null;
        } catch (e) {
            this.sendError('Spotify API error: ' + e);
            return null;
        }
    }

    private async getSpotifyAccessToken(): Promise<string | null> {
        const clientId = process.env.SPOTIFY_CLIENT_ID;
        const clientSecret = process.env.SPOTIFY_CLIENT_SECRET;
        if (!clientId || !clientSecret) return null;
        const creds = Buffer.from(`${clientId}:${clientSecret}`).toString('base64');
        try {
            const resp = await axios.post('https://accounts.spotify.com/api/token', 'grant_type=client_credentials', {
                headers: {
                    Authorization: `Basic ${creds}`,
                    'Content-Type': 'application/x-www-form-urlencoded',
                },
            });
            return resp.data.access_token;
        } catch (e) {
            this.sendError('Spotify token error: ' + e);
            return null;
        }
    }

    // Debug WAV file writing methods
    private writeOriginalWavFile(): void {
        if (this.hasWrittenOriginalWav) return;

        try {
            const combinedAudio = Buffer.concat(this.originalAudioBuffer);
            const wavBuffer = this.createWavBufferForDebug(combinedAudio, INPUT_SAMPLE_RATE);

            const filename = `debug_esp32_${this.sessionId}_16000Hz.wav`;
            fs.writeFileSync(filename, wavBuffer);

            this.log('info', `Saved ESP32 audio to ${filename} (${combinedAudio.length} bytes, ~${(combinedAudio.length / 2 / INPUT_SAMPLE_RATE).toFixed(1)}s)`);
            this.hasWrittenOriginalWav = true;
        } catch (error) {
            this.log('error', `Failed to write original WAV file: ${error}`);
        }
    }

    private writeResampledWavFile(): void {
        if (this.hasWrittenResampledWav) return;

        try {
            const combinedAudio = Buffer.concat(this.resampledAudioBuffer);
            const sampleRate = USE_RESAMPLING ? TARGET_SAMPLE_RATE : INPUT_SAMPLE_RATE;
            const wavBuffer = this.createWavBufferForDebug(combinedAudio, sampleRate);

            const filename = `debug_processed_${this.sessionId}_${sampleRate}Hz.wav`;
            fs.writeFileSync(filename, wavBuffer);

            this.log('info', `Saved processed audio to ${filename} (${combinedAudio.length} bytes, ~${(combinedAudio.length / 2 / sampleRate).toFixed(1)}s)`);
            this.hasWrittenResampledWav = true;
        } catch (error) {
            this.log('error', `Failed to write processed WAV file: ${error}`);
        }
    }

    private createWavBufferForDebug(pcmData: Buffer, sampleRate: number): Buffer {
        const numChannels = 1;
        const bitsPerSample = 16;
        const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        const blockAlign = numChannels * (bitsPerSample / 8);
        const dataSize = pcmData.length;
        const fileSize = 36 + dataSize;

        const buffer = Buffer.alloc(44);

        // RIFF header
        buffer.write('RIFF', 0);
        buffer.writeUInt32LE(fileSize, 4);
        buffer.write('WAVE', 8);

        // "fmt " sub-chunk
        buffer.write('fmt ', 12);
        buffer.writeUInt32LE(16, 16); // Sub-chunk size (16 for PCM)
        buffer.writeUInt16LE(1, 20); // Audio format (1 for PCM)
        buffer.writeUInt16LE(numChannels, 22);
        buffer.writeUInt32LE(sampleRate, 24);
        buffer.writeUInt32LE(byteRate, 28);
        buffer.writeUInt16LE(blockAlign, 32);
        buffer.writeUInt16LE(bitsPerSample, 34);

        // "data" sub-chunk
        buffer.write('data', 36);
        buffer.writeUInt32LE(dataSize, 40);

        return Buffer.concat([buffer, pcmData]);
    }
}