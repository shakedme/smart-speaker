import { ParsedUplinkFrame } from '../audio/uplink-frame';
import { SpeakerRouter } from './speaker-router';

const DEFAULT_FRAME_RATE_HZ = 20;
const DEFAULT_BUCKET_COUNT = 24;

export class PlaybackVisualizationPublisher {
  private readonly intervalMs: number;
  private readonly buckets: number[] = [];
  private lastPublishMs = 0;

  constructor(
    private readonly speakerId: string,
    private readonly router: SpeakerRouter,
    private readonly frameRateHz = DEFAULT_FRAME_RATE_HZ,
    private readonly bucketCount = DEFAULT_BUCKET_COUNT,
  ) {
    this.intervalMs = Math.max(33, Math.round(1000 / Math.max(1, this.frameRateHz)));
  }

  pushPlaybackRef(frame: ParsedUplinkFrame): void {
    if (frame.pcm.length < 2) {
      return;
    }
    this.buckets.push(this.peakBucket(frame.pcm));
    while (this.buckets.length > this.bucketCount) {
      this.buckets.shift();
    }

    const now = Date.now();
    if (now - this.lastPublishMs < this.intervalMs) {
      return;
    }
    this.lastPublishMs = now;
    this.router.publishWaveform({
      type: 'ui.waveform',
      speaker_id: this.speakerId,
      source: 'playback_ref',
      format: 'peak_buckets',
      sample_rate_hz: this.frameRateHz,
      channels: 1,
      buckets: [...this.buckets],
    });
  }

  private peakBucket(pcm: Buffer): number {
    let peak = 0;
    for (let offset = 0; offset + 1 < pcm.length; offset += 2) {
      const sample = Math.abs(pcm.readInt16LE(offset));
      if (sample > peak) {
        peak = sample;
      }
    }
    return Math.min(1, peak / 32768);
  }
}
