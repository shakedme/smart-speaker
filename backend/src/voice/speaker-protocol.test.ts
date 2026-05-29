import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { parseControlMessage, SPEAKER_PROTOCOL_VERSION, validateHello } from './speaker-protocol';

describe('speaker protocol', () => {
  it('accepts a valid playback node hello', () => {
    const message = parseControlMessage(Buffer.from(JSON.stringify({
      type: 'hello',
      protocol_version: SPEAKER_PROTOCOL_VERSION,
      speaker_id: 'rufus-001',
      node_id: 'wrover-main',
      node_role: 'playback_node',
      board_profile: 'esp32_wrover_playback',
      capabilities: ['spotify', 'llm_audio_downlink', 'playback_ref'],
    })));

    assert.ok(message);
    const result = validateHello(message);
    assert.equal(result.ok, true);
    if (result.ok) {
      assert.equal(result.hello.speaker_id, 'rufus-001');
      assert.equal(result.hello.node_role, 'playback_node');
    }
  });

  it('rejects non-v2 hello messages', () => {
    const message = parseControlMessage(Buffer.from(JSON.stringify({
      type: 'hello',
      protocol_version: 1,
      speaker_id: 'rufus-001',
      node_id: 'legacy',
      node_role: 'voice_ui_node',
      board_profile: 'esp32_s3_korvo2_lcd',
      capabilities: ['mic_uplink'],
    })));

    assert.ok(message);
    const result = validateHello(message);
    assert.equal(result.ok, false);
    if (!result.ok) {
      assert.match(result.error, /unsupported protocol_version/);
    }
  });
});
