import { SessionState } from './session-state';
import { SpeakerNodeConnection } from './speaker-node-connection';
import { AppLoggerService } from '../logging/logger.module';
import { NowPlayingState, WaveformState } from './speaker-protocol';

export class SpeakerRouter {
  private playbackNode: SpeakerNodeConnection | null = null;
  private voiceUiNode: SpeakerNodeConnection | null = null;
  private readonly log: ReturnType<AppLoggerService['forPrefix']>;

  constructor(
    private readonly speakerId: string,
    logger: AppLoggerService,
  ) {
    this.log = logger.forPrefix('ROUTER', speakerId);
  }

  attach(node: SpeakerNodeConnection): void {
    if (node.role === 'playback_node') {
      this.playbackNode = node;
    } else {
      this.voiceUiNode = node;
    }
  }

  detach(node: SpeakerNodeConnection): void {
    if (this.playbackNode === node) {
      this.playbackNode = null;
    }
    if (this.voiceUiNode === node) {
      this.voiceUiNode = null;
    }
  }

  hasPlaybackNode(): boolean {
    return this.playbackNode?.isOpen === true;
  }

  hasVoiceUiNode(): boolean {
    return this.voiceUiNode?.isOpen === true;
  }

  sendStateChange(state: SessionState): void {
    this.voiceUiNode?.sendStateChange(state);
    this.playbackNode?.sendStateChange(state);
  }

  sendIntent(intent: 'spotify' | 'playback_control', data: string): void {
    if (!this.playbackNode?.isOpen) {
      this.log.warn(`[ROUTER] dropping ${intent} command: playback_node unavailable`);
      return;
    }
    this.log.info(`[ROUTER] sending ${intent} command to playback_node node_id=${this.playbackNode.nodeId}`);
    this.playbackNode.sendPlaybackIntent(intent, data);
  }

  sendError(message: string): void {
    this.voiceUiNode?.sendError(message);
    this.playbackNode?.sendError(message);
  }

  sendPcmChunks(audioBuffer: Buffer): void {
    if (!this.playbackNode?.isOpen) {
      this.log.warn('[ROUTER] dropping llm_audio: playback_node unavailable');
      return;
    }
    this.log.debug(`[ROUTER] sending llm_audio to playback_node node_id=${this.playbackNode.nodeId}`);
    this.playbackNode.sendPcmChunks(audioBuffer);
  }

  publishNowPlaying(state: NowPlayingState): void {
    if (!this.voiceUiNode?.isOpen) {
      return;
    }
    this.log.debug(`[ROUTER] now_playing update to voice_ui_node node_id=${this.voiceUiNode.nodeId}`);
    this.voiceUiNode.sendNowPlaying(state);
  }

  publishWaveform(state: WaveformState): void {
    if (!this.voiceUiNode?.isOpen) {
      return;
    }
    this.voiceUiNode.sendWaveform(state);
  }
}
