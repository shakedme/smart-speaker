import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { parseClassifiedIntent } from './intent-types';

describe('parseClassifiedIntent', () => {
  it('parses legacy string data for spotify', () => {
    const result = parseClassifiedIntent({
      intent: 'spotify',
      data: 'Iron and Wine',
    });
    assert.equal(result.intent, 'spotify');
    if (result.intent === 'spotify') {
      assert.equal(result.data, 'Iron and Wine');
    }
  });

  it('parses legacy string data for playback_control', () => {
    const result = parseClassifiedIntent({
      intent: 'playback_control',
      data: 'pause',
    });
    assert.equal(result.intent, 'playback_control');
    if (result.intent === 'playback_control') {
      assert.equal(result.data, 'pause');
    }
  });

  it('still accepts object-shaped spotify fallback from Gemini', () => {
    const result = parseClassifiedIntent({
      spotify: { query: 'Iron and Wine', originalText: 'play Iron and Wine' },
    });
    assert.equal(result.intent, 'spotify');
    if (result.intent === 'spotify') {
      assert.equal(result.data, 'Iron and Wine');
    }
  });

  it('parses tool_request object data', () => {
    const result = parseClassifiedIntent({
      intent: 'tool_request',
      data: { tool: 'get_daily_agenda', args: { date: 'today', timezone: 'UTC' } },
    });
    assert.equal(result.intent, 'tool_request');
    if (result.intent === 'tool_request') {
      assert.equal(result.data.tool, 'get_daily_agenda');
    }
  });
});
