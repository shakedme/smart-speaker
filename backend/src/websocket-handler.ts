import { WebSocket } from 'ws';
import { v4 as uuidv4 } from 'uuid';
import { GoogleGenerativeAI } from '@google/generative-ai';
import { TextToSpeechClient } from '@google-cloud/text-to-speech';
import fs from 'fs';
import path from 'path';
import { promisify } from 'util';

interface ControlMessage {
    type: 'start_recording' | 'stop_recording' | 'cancel' | 'audio_start' | 'audio_end' | 'error';
    message?: string;
}

const writeFile = promisify(fs.writeFile);

export class WebSocketHandler {
    private ws: WebSocket;
    private sessionId: string;
    private isRecording: boolean = false;
    private audioBuffer: Buffer[] = [];
    private gemini: GoogleGenerativeAI;
    private ttsClient: TextToSpeechClient;

    constructor(ws: WebSocket) {
        this.ws = ws;
        this.sessionId = uuidv4();
        
        // Initialize Google Gemini client for audio processing and LLM
        this.gemini = new GoogleGenerativeAI(process.env.GOOGLE_AI_API_KEY || '');

        // Initialize Google Cloud Text-to-Speech client
        this.ttsClient = new TextToSpeechClient();

        console.log(`Session ${this.sessionId} created`);
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
            console.log(`Session ${this.sessionId}: Received control message:`, message.type);

            switch (message.type) {
                case 'start_recording':
                    this.startRecording();
                    break;
                case 'stop_recording':
                    this.stopRecording();
                    break;
                case 'cancel':
                    this.cancelRecording();
                    break;
                default:
                    console.warn(`Session ${this.sessionId}: Unknown control message:`, message.type);
            }
        } catch (error) {
            console.error(`Session ${this.sessionId}: Failed to parse control message:`, error);
            this.sendError('Invalid control message format');
        }
    }

    private handleAudioData(data: Buffer): void {
        if (!this.isRecording) {
            console.warn(`Session ${this.sessionId}: Received audio data while not recording`);
            return;
        }

        this.audioBuffer.push(data);
        console.log(`Session ${this.sessionId}: Received audio chunk: ${data.length} bytes (total: ${this.audioBuffer.length} chunks)`);
    }

    private startRecording(): void {
        if (this.isRecording) {
            console.warn(`Session ${this.sessionId}: Already recording`);
            return;
        }

        this.isRecording = true;
        this.audioBuffer = [];
        console.log(`Session ${this.sessionId}: Started recording session`);
    }

    private async stopRecording(): Promise<void> {
        if (!this.isRecording) {
            console.warn(`Session ${this.sessionId}: Not currently recording`);
            return;
        }

        this.isRecording = false;
        console.log(`Session ${this.sessionId}: Stopped recording, processing ${this.audioBuffer.length} chunks`);

        if (this.audioBuffer.length === 0) {
            console.warn(`Session ${this.sessionId}: No audio data to process`);
            return;
        }

        try {
            await this.processAudioPipeline();
        } catch (error) {
            console.error(`Session ${this.sessionId}: Error processing audio:`, error);
            this.sendError('Failed to process audio');
        }
    }

    private cancelRecording(): void {
        this.isRecording = false;
        this.audioBuffer = [];
        console.log(`Session ${this.sessionId}: Cancelled recording session`);
    }

    private async processAudioPipeline(): Promise<void> {
        console.log(`Session ${this.sessionId}: Starting Gemini Audio→LLM→TTS pipeline`);

        // Step 1: Combined Audio Processing + LLM using Gemini
        const response = await this.processAudioWithGemini();
        if (!response) {
            this.sendError('Failed to process audio or generate response');
            return;
        }

        console.log(`Session ${this.sessionId}: Gemini response: "${response}"`);

        // Step 2: Text-to-Speech using Google Cloud TTS
        await this.textToSpeech(response);
    }


    private async processAudioWithGemini(): Promise<string | null> {
        const startTime = Date.now();

        try {
            // Combine all audio chunks
            const combinedAudio = Buffer.concat(this.audioBuffer);
            console.log(`Session ${this.sessionId}: Processing ${combinedAudio.length} bytes of audio with Gemini`);

            // Convert raw PCM to WAV format in memory
            const wavHeader = this.createWavHeader(combinedAudio.length);
            const wavFile = Buffer.concat([wavHeader, combinedAudio]);

            // Convert audio to base64 for Gemini
            const audioBase64 = wavFile.toString('base64');

            // Get the generative model
            const model = this.gemini.getGenerativeModel({
                model: "gemini-2.0-flash",
                generationConfig: {
                    maxOutputTokens: 150,
                    temperature: 0.7,
                }
            });

            // Prompt for voice assistant with audio input
            const prompt = `You are a helpful voice assistant for a smart Bluetooth speaker. 
            Listen to the audio and provide a concise, natural response suitable for voice output. 
            Keep responses under 100 words and conversational in tone.
            You can help with general questions, check weather, tell jokes, and provide information.`;

            // Send audio and prompt to Gemini
            const result = await model.generateContent([
                prompt,
                {
                    inlineData: {
                        mimeType: "audio/wav",
                        data: audioBase64,
                    },
                },
            ]);

            const response = result.response.text().trim();
            return response.length > 0 ? response : null;

        } catch (error) {
            const endTime = Date.now() - startTime;
            console.error(`Session ${this.sessionId}: Gemini audio processing took ${endTime}ms`);
            console.error(`Session ${this.sessionId}: Gemini audio error:`, error);
            return null;
        }
    }

    private createWavHeader(dataLength: number): Buffer {
        const header = Buffer.alloc(44);
    
        // WAV header for 16kHz, 16-bit, mono PCM
        const sampleRate = 16000;
        const numChannels = 1;
        const bitsPerSample = 16;
        const byteRate = sampleRate * numChannels * bitsPerSample / 8;
        const blockAlign = numChannels * bitsPerSample / 8;
    
        header.write('RIFF', 0);
        header.writeUInt32LE(36 + dataLength, 4);
        header.write('WAVE', 8);
        header.write('fmt ', 12);
        header.writeUInt32LE(16, 16); // PCM format
        header.writeUInt16LE(1, 20); // PCM
        header.writeUInt16LE(numChannels, 22); // Mono
        header.writeUInt32LE(sampleRate, 24); // Sample rate
        header.writeUInt32LE(byteRate, 28); // Byte rate
        header.writeUInt16LE(blockAlign, 32); // Block align (MISSING IN YOUR CODE)
        header.writeUInt16LE(bitsPerSample, 34); // Bits per sample
        header.write('data', 36);
        header.writeUInt32LE(dataLength, 40);
    
        return header;
    }



    private async textToSpeech(text: string): Promise<void> {
        try {
            console.log(`Session ${this.sessionId}: Converting to speech with Google Cloud TTS: "${text}"`);

            this.sendControlMessage({ type: 'audio_start' });

            // Call Google Cloud Text-to-Speech API
            const request = {
                input: { text: text },
                voice: {
                    languageCode: 'en-US',
                    name: 'en-US-Neural2-F', // High-quality neural voice
                    ssmlGender: 'FEMALE' as const
                },
                audioConfig: {
                    audioEncoding: 'MP3' as const,
                    speakingRate: 1.0,
                    pitch: 0.0,
                    volumeGainDb: 0.0
                },
            };

            const [response] = await this.ttsClient.synthesizeSpeech(request);

            if (!response.audioContent) {
                throw new Error('No audio content received from TTS service');
            }

            // Convert the response to a buffer
            const audioBuffer = Buffer.from(response.audioContent);

            // Send the complete audio immediately for fastest playback
            this.ws.send(audioBuffer);

            this.sendControlMessage({ type: 'audio_end' });
            console.log(`Session ${this.sessionId}: Finished sending audio response`);

        } catch (error) {
            console.error(`Session ${this.sessionId}: TTS error:`, error);
            this.sendError('Failed to generate speech');
        }
    }

    private sendControlMessage(message: ControlMessage): void {
        if (this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(message));
        }
    }

    private sendError(message: string): void {
        console.error(`Session ${this.sessionId}: Error: ${message}`);
        this.sendControlMessage({ type: 'error', message });
    }

    public cleanup(): void {
        this.isRecording = false;
        this.audioBuffer = [];
        
        console.log(`Session ${this.sessionId}: Cleaned up`);
    }
} 