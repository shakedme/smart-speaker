import { WebSocket } from 'ws';
import { GoogleGenAI } from '@google/genai';
import { pcmQuickStats } from '../audio/pcm-utils';
import { AudioCaptureRecorder } from '../audio/audio-capture-recorder';
import { BackendAec3Processor } from '../audio/backend-aec3-processor';
import {
  isFramedUplink,
  parseFramedUplink,
  UPLINK_FRAME_TYPE_MIC,
  UPLINK_FRAME_TYPE_PLAYBACK_REF,
} from '../audio/uplink-frame';
import { UplinkDecoder } from '../audio/uplink-decoder';
import {
  CONVERSATION_TIMEOUT_MS,
  createWavBuffer,
  WAKE_PHRASE_HINT,
} from '../audio/audio-buffer-utils';
import { VadDetector } from './vad-detector';
import { WakewordDetector } from './wakeword-detector';
import { DeviceMessageSender } from './device-message-sender';
import { isRecordingState, SessionState } from './session-state';
import { AppConfigService } from '../config/app-config.service';
import { AppLoggerService } from '../logging/logger.module';
import { AgentOrchestratorService } from '../agent/agent-orchestrator.service';
import { GeminiIntentClassifierService } from '../intent/gemini-intent-classifier.service';
import { ResponseService } from '../agent/response-service';
import { AgentResult } from '../agent/agent-types';

const UPLINK_RX_DIAG_INTERVAL_MS = 5000;
const UPLINK_RX_BURST_GAP_MS = 5;
const UPLINK_RX_STALL_GAP_MS = 150;
const UPLINK_EDGE_DIAG_THROTTLE_MS = 3000;
const VAD_DIAG_THROTTLE_MS = parseInt(process.env.VAD_DIAG_THROTTLE_MS ?? '1000', 10);
const WAKEWORD_GATE_DIAG_THROTTLE_MS = parseInt(process.env.WAKEWORD_GATE_DIAG_THROTTLE_MS ?? '800', 10);
const VAD_LOW_GAIN_WARN = process.env.VAD_LOW_GAIN_WARN !== '0' && process.env.VAD_LOW_GAIN_WARN !== 'false';
const VAD_LOW_GAIN_WARN_PEAK_DBFS = parseFloat(process.env.VAD_LOW_GAIN_WARN_PEAK_DBFS ?? '-42');
const VAD_LOW_GAIN_WARN_INTERVAL_MS = parseInt(process.env.VAD_LOW_GAIN_WARN_INTERVAL_MS ?? '5000', 10);

export interface SpeakerSessionDeps {
  ws: WebSocket;
  sessionId: string;
  deviceSender: DeviceMessageSender;
  vadDetector: VadDetector;
  wakewordDetector: WakewordDetector;
  aecProcessor: BackendAec3Processor | null;
  config: AppConfigService;
  logger: AppLoggerService;
  agentOrchestrator: AgentOrchestratorService;
  intentClassifier: GeminiIntentClassifierService;
  gemini: GoogleGenAI;
  createResponseService: (callbacks: {
    onTurnComplete: () => void;
    onSessionClosed: (sessionState: SessionState) => void;
    onGeminiSpeakingChange: (speaking: boolean) => void;
    getSessionState: () => SessionState;
  }) => ResponseService;
}

export class SpeakerSession {
  private readonly ws: WebSocket;
  private readonly sessionId: string;
  private readonly deviceSender: DeviceMessageSender;
  private readonly vadDetector: VadDetector;
  private readonly wakewordDetector: WakewordDetector;
  private readonly aecProcessor: BackendAec3Processor | null;
  private readonly config: AppConfigService;
  private readonly log: ReturnType<AppLoggerService['forPrefix']>;
  private readonly agentOrchestrator: AgentOrchestratorService;
  private readonly intentClassifier: GeminiIntentClassifierService;
  private readonly createResponseService: SpeakerSessionDeps['createResponseService'];

  private responseService!: ResponseService;
  private sessionState: SessionState = 'IDLE';
  private audioBuffer: Buffer[] = [];
  private silenceTimer: NodeJS.Timeout | null = null;
  private wakeWordDetected = false;
  private voiceActivityDetected = false;
  private wakeWordInferenceScheduled = false;
  private conversationTimer: NodeJS.Timeout | null = null;
  private isGeminiSpeaking = false;
  private uplinkDecoder = new UplinkDecoder();
  private uplinkProcessingChain: Promise<void> = Promise.resolve();
  private audioCapture: AudioCaptureRecorder | null = null;
  private readonly captureOnlyMode: boolean;

  private uplinkRxWindowStartMs = Date.now();
  private uplinkRxLastPacketMs = 0;
  private uplinkRxPackets = 0;
  private uplinkRxBytes = 0;
  private uplinkRxGapSumMs = 0;
  private uplinkRxGapCount = 0;
  private uplinkRxGapMaxMs = 0;
  private uplinkRxBurstPackets = 0;
  private uplinkRxStallPackets = 0;
  private aecDiagLastMs = 0;
  private vadDiagLastMs = 0;
  private wakewordGateDiagLastMs = 0;
  private uplinkNotRecordingDiagLastMs = 0;
  private decodeEmptyDiagLastMs = 0;
  private silentChunkDiagLastMs = 0;
  private vadLowGainWarnLastMs = 0;
  private oneShotSpokenResponse = false;

  constructor(deps: SpeakerSessionDeps) {
    this.ws = deps.ws;
    this.sessionId = deps.sessionId;
    this.deviceSender = deps.deviceSender;
    this.vadDetector = deps.vadDetector;
    this.wakewordDetector = deps.wakewordDetector;
    this.aecProcessor = deps.aecProcessor;
    this.config = deps.config;
    this.log = deps.logger.forPrefix('SESSION', deps.sessionId);
    this.agentOrchestrator = deps.agentOrchestrator;
    this.intentClassifier = deps.intentClassifier;
    this.createResponseService = deps.createResponseService;

    if (this.aecProcessor) {
      this.log.info(`Backend WebRTC AEC3 enabled (initial delay ${this.config.aecInitialDelayMs} ms)`);
    }

    this.audioCapture = AudioCaptureRecorder.createFromEnv(this.sessionId);
    this.captureOnlyMode = this.audioCapture !== null;
    if (this.audioCapture) {
      this.log.info(`Wake word capture enabled: ${this.audioCapture.outputPath}`);
      this.log.info('Capture-only mode: VAD and wake word detection disabled');
    }
  }

  async initialize(): Promise<void> {
    this.responseService = this.createResponseService({
      onTurnComplete: () => {
        if (this.oneShotSpokenResponse) {
          this.oneShotSpokenResponse = false;
          this.responseService.endConversation();
          this.resetToListening();
          return;
        }
        this.enterWaitingForUser();
      },
      onSessionClosed: (state) => this.handleLiveSessionClosed(state),
      onGeminiSpeakingChange: (speaking) => {
        this.isGeminiSpeaking = speaking;
      },
      getSessionState: () => this.sessionState,
    });

    this.log.info(`Silero VAD frame size: ${this.vadDetector.frameLength * 2} bytes`);
    this.broadcastStateChange('LISTENING');
  }

  handleMessage(data: Buffer, isBinary: boolean): void {
    if (!isBinary) {
      this.log.warn(`Received unexpected text message: ${data.toString()}`);
      return;
    }
    this.recordUplinkReceive(data.length);
    if (isFramedUplink(data)) {
      this.handleFramedUplink(data);
      return;
    }
    if (!this.isRecording) {
      this.maybeLogUplinkIgnoredNotRecording(data.length);
    }
    if (this.isRecording) {
      try {
        const pcm = this.uplinkDecoder.decode(data);
        if (pcm.length > 0) {
          if (this.captureOnlyMode) {
            this.audioCapture?.append(pcm);
            return;
          }
          this.uplinkProcessingChain = this.uplinkProcessingChain
            .then(() => this.handleAudioData(pcm))
            .catch((error) => {
              this.log.error(`Failed to process uplink audio: ${error}`);
            });
        } else if (this.config.voiceDebug) {
          this.maybeLogDecodeEmptyPcm(data);
        }
      } catch (error) {
        this.log.error(`Failed to decode uplink audio: ${error}`);
      }
    }
  }

  async cleanup(): Promise<void> {
    this.log.info('Cleaning up session...');
    await this.uplinkProcessingChain.catch(() => undefined);
    this.responseService.endConversation();
    this.clearAllTimers();
    this.audioCapture?.close();
    this.audioCapture = null;
    this.aecProcessor?.destroy();
    this.sessionState = 'IDLE';
    this.vadDetector.destroy();
  }

  private handleFramedUplink(data: Buffer): void {
    const frame = parseFramedUplink(data);
    if (!frame) {
      this.log.warn(`[UPLINK] invalid framed packet len=${data.length}`);
      return;
    }

    if (frame.frameType === UPLINK_FRAME_TYPE_PLAYBACK_REF) {
      this.aecProcessor?.pushFrame(frame);
      this.maybeLogAecDiagnostics();
      return;
    }

    if (frame.frameType !== UPLINK_FRAME_TYPE_MIC) {
      this.log.debug(`[UPLINK] ignoring framed type=${frame.frameType}`);
      return;
    }

    const pcmChunks = this.aecProcessor ? this.aecProcessor.pushFrame(frame) : [frame.pcm];

    if (!this.isRecording) {
      this.maybeLogUplinkIgnoredNotRecording(data.length);
      return;
    }

    for (const pcm of pcmChunks) {
      if (pcm.length === 0) {
        continue;
      }
      if (this.captureOnlyMode) {
        this.audioCapture?.append(pcm);
        continue;
      }
      this.uplinkProcessingChain = this.uplinkProcessingChain
        .then(() => this.handleAudioData(pcm))
        .catch((error) => {
          this.log.error(`Failed to process framed uplink audio: ${error}`);
        });
    }
    this.maybeLogAecDiagnostics();
  }

  private async handleAudioData(data: Buffer): Promise<void> {
    const hasAudio = data.some((byte) => byte !== 0);
    let speechActive = false;

    if (hasAudio) {
      const vadResult = await this.vadDetector.processPcm(data);
      speechActive = vadResult.speechActive;
      if (speechActive) {
        this.voiceActivityDetected = true;
      }

      const shouldLevelDiag = this.config.voiceDebug || VAD_LOW_GAIN_WARN;
      const now = Date.now();
      if (shouldLevelDiag && now - this.vadDiagLastMs >= VAD_DIAG_THROTTLE_MS) {
        this.vadDiagLastMs = now;
        const pcmStat = pcmQuickStats(data);
        if (this.config.voiceDebug) {
          const statStr = pcmStat
            ? `pk_dbfs=${pcmStat.peakDbfs.toFixed(1)} rms_dbfs=${pcmStat.rmsDbfs.toFixed(1)}`
            : 'pcm_stat=n/a';
          this.log.debug(
            `[VAD] prob=${vadResult.voiceProbability.toFixed(3)} speech=${speechActive ? 1 : 0} ${statStr}`,
          );
        }
        if (
          VAD_LOW_GAIN_WARN &&
          pcmStat &&
          pcmStat.peakDbfs < VAD_LOW_GAIN_WARN_PEAK_DBFS &&
          !speechActive &&
          now - this.vadLowGainWarnLastMs >= VAD_LOW_GAIN_WARN_INTERVAL_MS
        ) {
          this.vadLowGainWarnLastMs = now;
          this.log.warn(`[VAD] uplink peak very low (pk_dbfs=${pcmStat.peakDbfs.toFixed(1)})`);
        }
      }
    } else if (this.config.voiceDebug) {
      const now = Date.now();
      if (now - this.silentChunkDiagLastMs >= UPLINK_EDGE_DIAG_THROTTLE_MS) {
        this.silentChunkDiagLastMs = now;
        this.log.debug(`[VAD] all_zero_pcm chunk_bytes=${data.length}`);
      }
    }

    if (!this.wakeWordDetected && !this.responseService.hasActiveSession()) {
      this.wakewordDetector.push(data);
      if (speechActive) {
        this.scheduleWakeWordInference();
      }
    } else if (this.responseService.hasActiveSession() || this.wakeWordDetected) {
      if (hasAudio) {
        this.audioBuffer.push(data);
      }
    }

    if (speechActive && (this.wakeWordDetected || this.responseService.hasActiveSession())) {
      this.resetConversationTimer();
      this.resetSilenceTimer();
    }
  }

  private scheduleWakeWordInference(): void {
    if (this.wakeWordDetected || !this.wakewordDetector.hasPendingInference()) {
      return;
    }
    if (this.wakeWordInferenceScheduled) {
      return;
    }
    this.wakeWordInferenceScheduled = true;
    void this.runWakeWordInferenceLoop();
  }

  private async runWakeWordInferenceLoop(): Promise<void> {
    try {
      while (!this.wakeWordDetected) {
        if (!this.wakewordDetector.hasPendingInference()) {
          break;
        }
        const inference = await this.wakewordDetector.runPendingInference();
        if (!inference) {
          break;
        }
        if (inference.detection) {
          this.onWakeWordDetected(inference.detection.score, inference.detection.preRollPcm);
          break;
        }
      }
    } catch (error) {
      this.log.error(`Wake word inference failed: ${error}`);
    } finally {
      this.wakeWordInferenceScheduled = false;
      if (!this.wakeWordDetected && this.wakewordDetector.hasPendingInference()) {
        this.scheduleWakeWordInference();
      }
    }
  }

  private onWakeWordDetected(score: number, preRollPcm: Buffer): void {
    this.log.info(`Wake word detected (score=${score.toFixed(3)}). Listening for command...`);
    this.wakeWordDetected = true;
    if (preRollPcm.length > 0) {
      this.audioBuffer.push(Buffer.from(preRollPcm));
    }
    if (this.voiceActivityDetected) {
      this.resetSilenceTimer();
    }
  }

  private get isRecording(): boolean {
    return isRecordingState(this.sessionState);
  }

  private resetSilenceTimer(): void {
    if (this.silenceTimer) {
      clearTimeout(this.silenceTimer);
    }
    this.silenceTimer = setTimeout(() => {
      if (
        (this.wakeWordDetected || this.responseService.hasActiveSession()) &&
        this.audioBuffer.length > 0
      ) {
        this.log.info('Silence detected. Processing accumulated audio.');
        void this.processIntent();
      } else {
        this.resetWakeWordState();
      }
    }, this.config.vadSilenceTimeoutMs);
  }

  private resetWakeWordState(): void {
    this.wakeWordDetected = false;
    this.voiceActivityDetected = false;
    this.wakewordDetector.reset();
    this.vadDetector.reset();
    this.clearAudioBuffers();
    this.log.debug(`Wake word state reset. Listening for "${WAKE_PHRASE_HINT}" again...`);
  }

  private resetConversationTimer(): void {
    if (this.conversationTimer) {
      clearTimeout(this.conversationTimer);
    }
    if (!this.responseService.hasActiveSession()) {
      return;
    }
    this.conversationTimer = setTimeout(() => {
      if (this.sessionState === 'STREAMING_RESPONSE' || this.sessionState === 'PROCESSING_INTENT') {
        this.resetConversationTimer();
        return;
      }
      if (!this.isGeminiSpeaking) {
        this.log.info('Conversation timeout. Closing session.');
        this.endConversation();
      } else {
        this.resetConversationTimer();
      }
    }, CONVERSATION_TIMEOUT_MS);
  }

  private enterWaitingForUser(): void {
    if (this.sessionState === 'WAITING_FOR_USER') {
      return;
    }
    this.isGeminiSpeaking = false;
    this.uplinkDecoder.resetAdpcmState();
    this.audioBuffer = [];
    this.vadDetector.reset();
    this.voiceActivityDetected = false;
    if (this.silenceTimer) {
      clearTimeout(this.silenceTimer);
      this.silenceTimer = null;
    }
    this.log.info('Ready for follow-up. Speak without saying the wake word again.');
    this.resetConversationTimer();
    this.broadcastStateChange('WAITING_FOR_USER');
  }

  private handleLiveSessionClosed(state: SessionState): void {
    if (state === 'LISTENING' || state === 'IDLE') {
      return;
    }
    if (state === 'PROCESSING_INTENT') {
      this.deviceSender.sendError('Conversation session ended unexpectedly.');
      this.resetToListening();
    } else if (state === 'STREAMING_RESPONSE' || state === 'WAITING_FOR_USER') {
      this.resetToListening();
    }
  }

  private broadcastStateChange(newState: SessionState): void {
    this.sessionState = newState;
    this.deviceSender.sendStateChange(newState);
    this.log.info(`State changed to: ${newState}`);
  }

  private async processIntent(): Promise<void> {
    const continuingSession = this.responseService.hasActiveSession();

    if (this.audioBuffer.length === 0) {
      if (!continuingSession) {
        this.resetWakeWordState();
      }
      return;
    }

    if (!this.isRecording && !continuingSession) {
      this.resetWakeWordState();
      return;
    }

    this.broadcastStateChange('PROCESSING_INTENT');
    if (this.silenceTimer) {
      clearTimeout(this.silenceTimer);
    }

    try {
      const fullAudio = Buffer.concat(this.audioBuffer);

      if (continuingSession) {
        this.responseService.continueConversation(fullAudio);
        this.clearAudioBuffers();
        this.broadcastStateChange('STREAMING_RESPONSE');
        this.resetConversationTimer();
        return;
      }

      const wavBuffer = createWavBuffer(fullAudio);
      const classified = await this.intentClassifier.classify(wavBuffer, this.config.userTimezone);
      await this.dispatchAgentResult(classified);
    } catch (error) {
      this.log.error(`Failed to process intent: ${error}`);
      this.deviceSender.sendError(`Could not determine intent: ${error}`);
      this.resetToListening();
    } finally {
      if (!continuingSession) {
        this.resetWakeWordState();
      }
    }
  }

  private async dispatchAgentResult(classified: import('../intent/intent-types').ClassifiedIntent): Promise<void> {
    const result: AgentResult = await this.agentOrchestrator.handleIntent(classified, {
      sessionId: this.sessionId,
      timezone: this.config.userTimezone,
    });

    switch (result.type) {
      case 'device_command':
        this.broadcastStateChange('HANDLING_COMMAND');
        this.deviceSender.sendIntent(result.command.intent, result.command.data);
        this.resetToListening();
        break;
      case 'spoken_response':
        this.broadcastStateChange('STREAMING_RESPONSE');
        this.oneShotSpokenResponse = true;
        await this.responseService.speakText(result.text);
        break;
      case 'live_response':
        this.broadcastStateChange('STREAMING_RESPONSE');
        await this.responseService.startLiveInformationResponse(result.prompt);
        this.clearAudioBuffers();
        break;
      case 'error':
        this.deviceSender.sendError(result.message);
        if (result.recoverable) {
          this.log.warn(`Recoverable agent error: ${result.message}`);
          this.resetToListening();
        } else {
          this.resetToListening();
        }
        break;
      default:
        this.resetToListening();
    }
  }

  private endConversation(): void {
    this.responseService.endConversation();
    this.resetToListening();
  }

  private resetToListening(): void {
    this.clearAllTimers();
    this.isGeminiSpeaking = false;
    this.resetWakeWordState();
    this.broadcastStateChange('LISTENING');
    this.log.info('Reset to listening state. Ready for next wake word.');
  }

  private clearAudioBuffers(): void {
    this.audioBuffer = [];
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

  private maybeLogAecDiagnostics(): void {
    if (!this.aecProcessor || !this.config.voiceDebug) {
      return;
    }
    const now = Date.now();
    if (now - this.aecDiagLastMs < UPLINK_RX_DIAG_INTERVAL_MS) {
      return;
    }
    this.aecDiagLastMs = now;
    const stats = this.aecProcessor.getStats();
    this.log.debug(
      `[AEC3] render_fed=${stats.renderFramesFed} capture_out=${stats.captureFramesProcessed}`,
    );
  }

  private maybeLogUplinkIgnoredNotRecording(rawLen: number): void {
    if (!this.config.voiceDebug) {
      return;
    }
    const now = Date.now();
    if (now - this.uplinkNotRecordingDiagLastMs < UPLINK_EDGE_DIAG_THROTTLE_MS) {
      return;
    }
    this.uplinkNotRecordingDiagLastMs = now;
    this.log.debug(`[UPLINK] ignoring ${rawLen}b session=${this.sessionState}`);
  }

  private maybeLogDecodeEmptyPcm(rawWs: Buffer): void {
    if (!this.config.voiceDebug) {
      return;
    }
    const now = Date.now();
    if (now - this.decodeEmptyDiagLastMs < UPLINK_EDGE_DIAG_THROTTLE_MS) {
      return;
    }
    this.decodeEmptyDiagLastMs = now;
    this.log.debug(`[UPLINK] decode yielded 0 PCM bytes raw_len=${rawWs.length}`);
  }

  private recordUplinkReceive(length: number): void {
    const now = Date.now();
    if (this.uplinkRxLastPacketMs > 0) {
      const gapMs = now - this.uplinkRxLastPacketMs;
      this.uplinkRxGapSumMs += gapMs;
      this.uplinkRxGapCount++;
      this.uplinkRxGapMaxMs = Math.max(this.uplinkRxGapMaxMs, gapMs);
      if (gapMs <= UPLINK_RX_BURST_GAP_MS) {
        this.uplinkRxBurstPackets++;
      } else if (gapMs >= UPLINK_RX_STALL_GAP_MS) {
        this.uplinkRxStallPackets++;
      }
    }
    this.uplinkRxLastPacketMs = now;
    this.uplinkRxPackets++;
    this.uplinkRxBytes += length;

    const elapsedMs = now - this.uplinkRxWindowStartMs;
    if (elapsedMs < UPLINK_RX_DIAG_INTERVAL_MS) {
      return;
    }
    this.uplinkRxWindowStartMs = now;
    this.uplinkRxPackets = 0;
    this.uplinkRxBytes = 0;
    this.uplinkRxGapSumMs = 0;
    this.uplinkRxGapCount = 0;
    this.uplinkRxGapMaxMs = 0;
    this.uplinkRxBurstPackets = 0;
    this.uplinkRxStallPackets = 0;
  }
}
