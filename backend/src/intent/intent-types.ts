export type PlaybackAction = 'play' | 'pause' | 'next' | 'previous' | 'stop' | 'resume';

/** Matches pre-refactor Gemini output: data is a string for core intents. */
export type ClassifiedIntent =
  | {
      intent: 'spotify';
      data: string;
    }
  | {
      intent: 'playback_control';
      data: string;
    }
  | {
      intent: 'information';
      data: string;
    }
  | {
      intent: 'tool_request';
      data: {
        tool: string;
        args: Record<string, unknown>;
      };
    }
  | {
      intent: 'unknown';
      data: string;
    };

const PLAYBACK_ACTIONS = new Set<string>(['play', 'pause', 'next', 'previous', 'stop', 'resume']);

/** Normalize object-shaped playback_control fallbacks only. */
export function normalizePlaybackAction(data: unknown): PlaybackAction {
  if (typeof data !== 'string') {
    return 'pause';
  }
  const lower = data.toLowerCase().trim();
  if (lower.includes('next') || lower.includes('skip')) {
    return 'next';
  }
  if (lower.includes('previous') || lower.includes('back')) {
    return 'previous';
  }
  if (lower.includes('pause')) {
    return 'pause';
  }
  if (lower.includes('stop')) {
    return 'stop';
  }
  if (lower.includes('resume') || lower.includes('continue')) {
    return 'resume';
  }
  if (lower.includes('play')) {
    return 'play';
  }
  if (PLAYBACK_ACTIONS.has(lower)) {
    return lower as PlaybackAction;
  }
  return 'pause';
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (typeof value === 'object' && value !== null && !Array.isArray(value)) {
    return value as Record<string, unknown>;
  }
  return null;
}

function asString(value: unknown): string | undefined {
  return typeof value === 'string' ? value : undefined;
}

function toolRequestFromRecord(dataRecord: Record<string, unknown>): ClassifiedIntent {
  const tool = asString(dataRecord.tool);
  if (!tool) {
    return { intent: 'unknown', data: 'tool_request missing tool name' };
  }
  const args = asRecord(dataRecord.args) ?? {};
  return { intent: 'tool_request', data: { tool, args } };
}

/** Parse Gemini JSON into ClassifiedIntent. */
export function parseClassifiedIntent(raw: unknown): ClassifiedIntent {
  const record = asRecord(raw);
  if (!record) {
    return { intent: 'unknown', data: 'invalid intent shape' };
  }

  if (typeof record.intent !== 'string') {
    const topLevelIntents = [
      'spotify',
      'playback_control',
      'information',
      'tool_request',
      'unknown',
    ] as const;
    for (const key of topLevelIntents) {
      if (key in record) {
        return parseClassifiedIntent({ intent: key, data: record[key] });
      }
    }
    return { intent: 'unknown', data: 'invalid intent shape' };
  }

  const intentType = record.intent;
  const data = record.data;

  if (typeof data === 'string') {
    switch (intentType) {
      case 'spotify':
      case 'playback_control':
      case 'information':
      case 'unknown':
        return { intent: intentType, data };
      case 'tool_request':
        return { intent: 'unknown', data: 'tool_request requires object data' };
      default:
        return { intent: 'unknown', data: `unknown intent type: ${intentType}` };
    }
  }

  const dataRecord = asRecord(data);
  if (!dataRecord) {
    return { intent: 'unknown', data: 'missing data' };
  }

  switch (intentType) {
    case 'spotify': {
      const text =
        asString(dataRecord.query) ??
        asString(dataRecord.originalText) ??
        asString(dataRecord.text) ??
        '';
      return { intent: 'spotify', data: text };
    }
    case 'playback_control': {
      const text =
        asString(dataRecord.action) ??
        asString(dataRecord.originalText) ??
        asString(dataRecord.text) ??
        '';
      return { intent: 'playback_control', data: text };
    }
    case 'information': {
      const text = asString(dataRecord.question) ?? asString(dataRecord.text) ?? '';
      return { intent: 'information', data: text };
    }
    case 'tool_request':
      return toolRequestFromRecord(dataRecord);
    case 'unknown': {
      return { intent: 'unknown', data: asString(dataRecord.reason) ?? 'unknown intent' };
    }
    default:
      return { intent: 'unknown', data: `unknown intent type: ${intentType}` };
  }
}
