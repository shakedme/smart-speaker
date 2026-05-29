import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { PlaybackVisualizationPublisher } from './playback-visualization-publisher';
import { UPLINK_FRAME_TYPE_PLAYBACK_REF } from '../audio/uplink-frame';
import { SpeakerRouter } from './speaker-router';

describe('PlaybackVisualizationPublisher', () => {
  it('publishes compact peak buckets from playback reference PCM', () => {
    const waveforms: unknown[] = [];
    const router = {
      publishWaveform: (payload: unknown) => waveforms.push(payload),
    } as unknown as SpeakerRouter;
    const publisher = new PlaybackVisualizationPublisher('rufus-001', router, 30, 4);
    const pcm = Buffer.alloc(8);
    pcm.writeInt16LE(0, 0);
    pcm.writeInt16LE(8192, 2);
    pcm.writeInt16LE(-16384, 4);
    pcm.writeInt16LE(32767, 6);

    publisher.pushPlaybackRef({
      frameType: UPLINK_FRAME_TYPE_PLAYBACK_REF,
      sequence: 1,
      timestampUs: 1000,
      sampleRate: 16000,
      channelCount: 1,
      numSamples: 4,
      pcm,
    });

    assert.equal(waveforms.length, 1);
    assert.deepEqual(waveforms[0], {
      type: 'ui.waveform',
      speaker_id: 'rufus-001',
      source: 'playback_ref',
      format: 'peak_buckets',
      sample_rate_hz: 30,
      channels: 1,
      buckets: [32767 / 32768],
    });
  });
});
