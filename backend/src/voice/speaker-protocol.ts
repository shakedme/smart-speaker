import { SessionState } from './session-state';

export const SPEAKER_PROTOCOL_VERSION = 2;

export type NodeRole = 'playback_node' | 'voice_ui_node';

export type BoardProfile =
  | 'esp32_wrover_playback'
  | 'esp32_s3_korvo2_lcd'
  | 'esp32_s3_n32r16_sph0645'
  | 'esp32_s3_n32r16_sph0645_lcd';

export type NodeCapability =
  | 'spotify'
  | 'bluetooth_a2dp'
  | 'speaker_output'
  | 'llm_audio_downlink'
  | 'playback_ref'
  | 'playback_state'
  | 'waveform_source'
  | 'mic_uplink'
  | 'display'
  | 'now_playing_ui'
  | 'waveform_ui';

export interface HelloMessage {
  type: 'hello';
  protocol_version: number;
  speaker_id: string;
  node_id: string;
  node_role: NodeRole;
  board_profile: BoardProfile | string;
  capabilities: NodeCapability[] | string[];
}

export interface NowPlayingState {
  type: 'ui.now_playing';
  speaker_id: string;
  source: 'spotify' | 'bluetooth' | 'llm' | 'unknown';
  playback_state: 'playing' | 'paused' | 'stopped' | 'idle' | 'unknown';
  title?: string;
  artist?: string;
  album?: string;
  duration_ms?: number;
  position_ms?: number;
  updated_at_ms: number;
}

export interface WaveformState {
  type: 'ui.waveform';
  speaker_id: string;
  source: 'playback_ref';
  format: 'peak_buckets';
  sample_rate_hz: number;
  channels: 1;
  buckets: number[];
}

export interface PlaybackStateMessage {
  type: 'playback.state';
  speaker_id: string;
  source: 'spotify' | 'bluetooth' | 'llm' | 'unknown';
  playback_state: 'playing' | 'paused' | 'stopped' | 'idle' | 'unknown';
  title?: string;
  artist?: string;
  album?: string;
  duration_ms?: number;
  position_ms?: number;
}

export interface SessionStateMessage {
  type: 'state_change';
  data: SessionState;
}

export type SpeakerControlMessage =
  | HelloMessage
  | PlaybackStateMessage
  | NowPlayingState
  | WaveformState
  | SessionStateMessage
  | { type: string; [key: string]: unknown };

export interface ValidHello {
  ok: true;
  hello: HelloMessage;
}

export interface InvalidHello {
  ok: false;
  error: string;
}

export function parseControlMessage(data: Buffer): SpeakerControlMessage | null {
  try {
    const parsed: unknown = JSON.parse(data.toString('utf8'));
    if (!parsed || typeof parsed !== 'object') {
      return null;
    }
    const type = (parsed as { type?: unknown }).type;
    if (typeof type !== 'string') {
      return null;
    }
    return parsed as SpeakerControlMessage;
  } catch {
    return null;
  }
}

export function validateHello(message: SpeakerControlMessage): ValidHello | InvalidHello {
  if (message.type !== 'hello') {
    return { ok: false, error: 'first text message must be hello' };
  }
  const hello = message as Partial<HelloMessage>;
  if (hello.protocol_version !== SPEAKER_PROTOCOL_VERSION) {
    return { ok: false, error: `unsupported protocol_version=${String(hello.protocol_version)}` };
  }
  if (!isNonEmptyString(hello.speaker_id)) {
    return { ok: false, error: 'speaker_id is required' };
  }
  if (!isNonEmptyString(hello.node_id)) {
    return { ok: false, error: 'node_id is required' };
  }
  if (hello.node_role !== 'playback_node' && hello.node_role !== 'voice_ui_node') {
    return { ok: false, error: 'node_role must be playback_node or voice_ui_node' };
  }
  if (!isNonEmptyString(hello.board_profile)) {
    return { ok: false, error: 'board_profile is required' };
  }
  if (!Array.isArray(hello.capabilities) || hello.capabilities.some((cap) => typeof cap !== 'string')) {
    return { ok: false, error: 'capabilities must be a string array' };
  }
  return { ok: true, hello: hello as HelloMessage };
}

function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.trim().length > 0;
}
