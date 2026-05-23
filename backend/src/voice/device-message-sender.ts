import { WebSocket } from 'ws';
import { SessionState } from './session-state';

const MIN_RESPONSE_AUDIO_BYTES = 320;
const ESP_DOWNLINK_PCM_CHUNK_BYTES = 4096;

export interface DeviceIntentCommand {
  type: 'intent';
  intent: 'spotify' | 'playback_control';
  data: string;
}

export interface DeviceErrorMessage {
  type: 'error';
  message: string;
}

export class DeviceMessageSender {
  constructor(private readonly ws: WebSocket) {}

  sendStateChange(state: SessionState): void {
    if (this.ws.readyState !== WebSocket.OPEN) {
      return;
    }
    this.ws.send(JSON.stringify({ type: 'state_change', data: state }));
  }

  sendIntent(intent: 'spotify' | 'playback_control', data: string): void {
    if (this.ws.readyState !== WebSocket.OPEN) {
      return;
    }
    const payload: DeviceIntentCommand = { type: 'intent', intent, data };
    this.ws.send(JSON.stringify(payload));
  }

  sendError(message: string): void {
    if (this.ws.readyState !== WebSocket.OPEN) {
      return;
    }
    const payload: DeviceErrorMessage = { type: 'error', message };
    this.ws.send(JSON.stringify(payload));
  }

  sendPcmChunks(audioBuffer: Buffer): void {
    if (audioBuffer.length < MIN_RESPONSE_AUDIO_BYTES) {
      return;
    }
    if (this.ws.readyState !== WebSocket.OPEN) {
      return;
    }
    for (let offset = 0; offset < audioBuffer.length; offset += ESP_DOWNLINK_PCM_CHUNK_BYTES) {
      const chunk = audioBuffer.subarray(
        offset,
        Math.min(offset + ESP_DOWNLINK_PCM_CHUNK_BYTES, audioBuffer.length),
      );
      this.ws.send(chunk);
    }
  }
}

export { MIN_RESPONSE_AUDIO_BYTES, ESP_DOWNLINK_PCM_CHUNK_BYTES };
