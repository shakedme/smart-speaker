import { GoogleGenAI, LiveServerMessage, Modality, Session } from '@google/genai';
import { DeviceMessageSender } from '../voice/device-message-sender';
import { AppLoggerService, LogLevel } from '../logging/logger.module';
import {
  CONVERSATION_TIMEOUT_MS,
  GEMINI_RESPONSE_PCM_SAMPLE_RATE,
  GEMINI_RESPONSE_VOICE,
  UPLINK_PCM_SAMPLE_RATE,
} from '../audio/audio-buffer-utils';
import { SessionState } from '../voice/session-state';

export interface ResponseServiceCallbacks {
  onTurnComplete: () => void;
  onSessionClosed: (sessionState: SessionState) => void;
  onGeminiSpeakingChange: (speaking: boolean) => void;
  getSessionState: () => SessionState;
}

export class ResponseService {
  private responseSession: Session | null = null;
  private isGeminiSpeaking = false;
  private responseAudioMimeLogged = false;
  private readonly log: Record<LogLevel, (message: string) => void>;

  constructor(
    private readonly gemini: GoogleGenAI,
    private readonly deviceSender: DeviceMessageSender,
    logger: AppLoggerService,
    private readonly sessionId: string,
    private readonly callbacks: ResponseServiceCallbacks,
  ) {
    this.log = logger.forPrefix('RESPONSE', sessionId);
  }

  hasActiveSession(): boolean {
    return this.responseSession !== null;
  }

  isSpeaking(): boolean {
    return this.isGeminiSpeaking;
  }

  async startLiveInformationResponse(question: string): Promise<void> {
    await this.connectLiveSession(
      `You are a helpful voice assistant. Provide concise, natural responses. Keep your responses conversational and be ready for follow-up questions.`,
      (session) => {
        session.sendClientContent({
          turns: [{ role: 'user', parts: [{ text: question }] }],
          turnComplete: true,
        });
      },
    );
  }

  async speakText(text: string): Promise<void> {
    const instruction =
      'You are a text-to-speech reader. Read the following text aloud verbatim. ' +
      'Do not add, omit, rephrase, or comment on the text.';
    await this.connectLiveSession(instruction, (session) => {
      session.sendClientContent({
        turns: [{ role: 'user', parts: [{ text }] }],
        turnComplete: true,
      });
    });
  }

  continueConversation(pcmAudio: Buffer): void {
    if (!this.responseSession) {
      throw new Error('No active live response session');
    }
    if (pcmAudio.length === 0) {
      throw new Error('Empty PCM audio for follow-up turn');
    }

    this.log.info(`Continuing conversation (${pcmAudio.length} bytes PCM follow-up).`);
    this.isGeminiSpeaking = false;
    this.callbacks.onGeminiSpeakingChange(false);

    this.responseSession.sendRealtimeInput({
      audio: {
        data: pcmAudio.toString('base64'),
        mimeType: `audio/pcm;rate=${UPLINK_PCM_SAMPLE_RATE}`,
      },
    });
  }

  endConversation(): void {
    this.log.info('Ending conversation session.');
    if (this.responseSession) {
      const session = this.responseSession;
      this.responseSession = null;
      session.close();
    }
    this.isGeminiSpeaking = false;
    this.callbacks.onGeminiSpeakingChange(false);
  }

  private async connectLiveSession(
    systemInstruction: string,
    sendInitial: (session: Session) => void,
  ): Promise<void> {
    try {
      this.responseSession = await this.gemini.live.connect({
        model: 'gemini-3.1-flash-live-preview',
        config: {
          responseModalities: [Modality.AUDIO],
          speechConfig: {
            languageCode: 'en-US',
            voiceConfig: {
              prebuiltVoiceConfig: {
                voiceName: GEMINI_RESPONSE_VOICE,
              },
            },
          },
          systemInstruction,
        },
        callbacks: {
          onopen: () => {
            this.log.info('Audio response session opened.');
          },
          onmessage: (message: LiveServerMessage) => this.handleAudioResponse(message),
          onerror: (e: unknown) => {
            this.log.error(`Audio response error: ${JSON.stringify(e)}`);
            this.endConversation();
            this.callbacks.onSessionClosed(this.callbacks.getSessionState());
          },
          onclose: (event: { code?: number; reason?: string }) => {
            const closeCode = event?.code ?? 'unknown';
            const closeReason = event?.reason || 'none';
            this.log.info(`Audio response session closed (code=${closeCode}, reason=${closeReason}).`);
            this.responseSession = null;
            this.callbacks.onSessionClosed(this.callbacks.getSessionState());
          },
        },
      });

      this.responseAudioMimeLogged = false;
      sendInitial(this.responseSession);
    } catch (error) {
      this.responseSession = null;
      this.log.error(`Failed to start audio response session: ${error}`);
      throw error;
    }
  }

  private decodeInlineBlobData(data: unknown): Buffer {
    if (typeof data === 'string') {
      return Buffer.from(data, 'base64');
    }
    if (Buffer.isBuffer(data)) {
      return data;
    }
    if (data instanceof Uint8Array) {
      return Buffer.from(data);
    }
    return Buffer.alloc(0);
  }

  private extractLiveAudioChunks(message: LiveServerMessage): Array<{ buffer: Buffer; mimeType?: string }> {
    const chunks: Array<{ buffer: Buffer; mimeType?: string }> = [];
    const parts = message.serverContent?.modelTurn?.parts ?? [];

    for (const part of parts) {
      const inlineData = part.inlineData;
      if (!inlineData?.data) {
        continue;
      }
      const buffer = this.decodeInlineBlobData(inlineData.data);
      if (buffer.length > 0) {
        chunks.push({
          buffer,
          ...(inlineData.mimeType ? { mimeType: inlineData.mimeType } : {}),
        });
      }
    }

    if (chunks.length === 0 && message.data) {
      const buffer = Buffer.from(message.data, 'base64');
      if (buffer.length > 0) {
        chunks.push({ buffer });
      }
    }

    return chunks;
  }

  private handleAudioResponse(message: LiveServerMessage): void {
    if (message.goAway) {
      this.log.warn(`Live session GoAway: ${JSON.stringify(message.goAway)}`);
    }

    const serverContent = message.serverContent;

    if (serverContent?.generationComplete) {
      this.log.info('Gemini generation complete.');
    }

    if (serverContent?.interrupted) {
      this.log.warn('Gemini response interrupted.');
    }

    for (const { buffer, mimeType } of this.extractLiveAudioChunks(message)) {
      this.sendAudioResponseToDevice(buffer, mimeType);
    }

    if (serverContent?.turnComplete) {
      this.log.info('Gemini turn complete. Ready for follow-up.');
      this.callbacks.onTurnComplete();
    }
  }

  private sendAudioResponseToDevice(audioBuffer: Buffer, mimeType?: string): void {
    if (!this.isGeminiSpeaking) {
      this.isGeminiSpeaking = true;
      this.callbacks.onGeminiSpeakingChange(true);
      this.log.info('Gemini started speaking.');
    }
    if (!this.responseAudioMimeLogged) {
      this.responseAudioMimeLogged = true;
      this.log.info(
        `Gemini response audio format: ${mimeType ?? `audio/pcm;rate=${GEMINI_RESPONSE_PCM_SAMPLE_RATE}`}; ` +
          `ESP playback expects ${GEMINI_RESPONSE_PCM_SAMPLE_RATE}Hz mono s16le; voice=${GEMINI_RESPONSE_VOICE}`,
      );
    }
    this.deviceSender.sendPcmChunks(audioBuffer);
  }
}

export { CONVERSATION_TIMEOUT_MS };
