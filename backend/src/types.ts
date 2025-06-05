export interface ControlMessage {
    type: 'start_recording' | 'stop_recording' | 'cancel' | 'audio_start' | 'audio_end' | 'error';
    message?: string;
}

export interface AudioChunk {
    data: Buffer;
    timestamp: number;
    sampleRate: number;
    channels: number;
    bitDepth: number;
}

export interface SessionInfo {
    sessionId: string;
    startTime: Date;
    isRecording: boolean;
    audioChunks: number;
    totalBytes: number;
}

export interface STTResponse {
    text: string;
    confidence: number;
    language?: string;
}

export interface LLMRequest {
    text: string;
    sessionId: string;
    context?: Record<string, any>;
}

export interface LLMResponse {
    text: string;
    model?: string;
    usage?: {
        promptTokens: number;
        completionTokens: number;
        totalTokens: number;
    };
}

export interface TTSRequest {
    text: string;
    voice?: string;
    speed?: number;
    pitch?: number;
}

export interface TTSResponse {
    audioData: Buffer;
    format: {
        sampleRate: number;
        channels: number;
        bitDepth: number;
    };
}

export interface ServerConfig {
    port: number;
    openaiApiKey?: string;
    googleCredentials?: string;
    nodeEnv: 'development' | 'production' | 'test';
}

export interface HealthResponse {
    status: 'ok' | 'error';
    timestamp: string;
    connections: number;
    uptime?: number;
    memory?: {
        used: number;
        total: number;
    };
} 