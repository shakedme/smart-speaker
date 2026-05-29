import { WebSocket } from 'ws';
import {
  HelloMessage,
  NodeRole,
  NowPlayingState,
  PlaybackStateMessage,
  SessionStateMessage,
  WaveformState,
} from './speaker-protocol';

const MIN_RESPONSE_AUDIO_BYTES = 320;
const ESP_DOWNLINK_PCM_CHUNK_BYTES = 4096;

export class SpeakerNodeConnection {
  readonly speakerId: string;
  readonly nodeId: string;
  readonly role: NodeRole;
  readonly boardProfile: string;
  readonly capabilities: readonly string[];

  constructor(
    readonly ws: WebSocket,
    readonly hello: HelloMessage,
  ) {
    this.speakerId = hello.speaker_id;
    this.nodeId = hello.node_id;
    this.role = hello.node_role;
    this.boardProfile = hello.board_profile;
    this.capabilities = hello.capabilities;
  }

  get isOpen(): boolean {
    return this.ws.readyState === WebSocket.OPEN;
  }

  sendJson(payload: object): void {
    if (!this.isOpen) {
      return;
    }
    this.ws.send(JSON.stringify(payload));
  }

  sendStateChange(state: SessionStateMessage['data']): void {
    this.sendJson({ type: 'state_change', data: state } satisfies SessionStateMessage);
  }

  sendPlaybackIntent(intent: 'spotify' | 'playback_control', data: string): void {
    this.sendJson({ type: 'intent', intent, data });
  }

  sendError(message: string): void {
    this.sendJson({ type: 'error', message });
  }

  sendNowPlaying(state: NowPlayingState): void {
    this.sendJson(state);
  }

  sendWaveform(state: WaveformState): void {
    this.sendJson(state);
  }

  sendPcmChunks(audioBuffer: Buffer): void {
    if (audioBuffer.length < MIN_RESPONSE_AUDIO_BYTES || !this.isOpen) {
      return;
    }
    for (let offset = 0; offset < audioBuffer.length; offset += ESP_DOWNLINK_PCM_CHUNK_BYTES) {
      const chunk = audioBuffer.subarray(offset, Math.min(offset + ESP_DOWNLINK_PCM_CHUNK_BYTES, audioBuffer.length));
      this.ws.send(chunk);
    }
  }
}

export function playbackStateToNowPlaying(message: PlaybackStateMessage): NowPlayingState {
  return {
    type: 'ui.now_playing',
    speaker_id: message.speaker_id,
    source: message.source,
    playback_state: message.playback_state,
    ...(message.title ? { title: message.title } : {}),
    ...(message.artist ? { artist: message.artist } : {}),
    ...(message.album ? { album: message.album } : {}),
    ...(typeof message.duration_ms === 'number' ? { duration_ms: message.duration_ms } : {}),
    ...(typeof message.position_ms === 'number' ? { position_ms: message.position_ms } : {}),
    updated_at_ms: Date.now(),
  };
}

export { MIN_RESPONSE_AUDIO_BYTES, ESP_DOWNLINK_PCM_CHUNK_BYTES };
