export type CalendarProviderType = 'mock' | 'none' | 'google' | 'microsoft';

export interface AppConfig {
  port: number;
  nodeEnv: string;
  googleAiApiKey: string;
  spotifyClientId: string;
  spotifyClientSecret: string;
  userTimezone: string;
  calendarProvider: CalendarProviderType;
  googleCalendarId: string;
  googleCalendarRefreshToken: string | undefined;
  googleCalendarIcsUrl: string | undefined;
  microsoftGraphAccessToken: string | undefined;
  vadThreshold: number;
  vadSilenceTimeoutMs: number;
  voiceDebug: boolean;
  backendAec3Enabled: boolean;
  aecInitialDelayMs: number;
}

function parseIntEnv(key: string, defaultValue: number): number {
  const raw = process.env[key];
  if (raw === undefined || raw === '') {
    return defaultValue;
  }
  const parsed = parseInt(raw, 10);
  return Number.isNaN(parsed) ? defaultValue : parsed;
}

function parseFloatEnv(key: string, defaultValue: number): number {
  const raw = process.env[key];
  if (raw === undefined || raw === '') {
    return defaultValue;
  }
  const parsed = parseFloat(raw);
  return Number.isNaN(parsed) ? defaultValue : parsed;
}

function parseCalendarProvider(raw: string | undefined): CalendarProviderType {
  switch (raw) {
    case 'mock':
    case 'none':
    case 'google':
    case 'microsoft':
      return raw;
    default:
      return 'mock';
  }
}

export function loadAppConfig(): AppConfig {
  const googleAiApiKey = process.env.GOOGLE_AI_API_KEY ?? '';
  if (!googleAiApiKey) {
    throw new Error('GOOGLE_AI_API_KEY is not set in environment variables.');
  }

  return {
    port: parseIntEnv('PORT', 8080),
    nodeEnv: process.env.NODE_ENV ?? 'development',
    googleAiApiKey,
    spotifyClientId: process.env.SPOTIFY_CLIENT_ID ?? '',
    spotifyClientSecret: process.env.SPOTIFY_CLIENT_SECRET ?? '',
    userTimezone: process.env.USER_TIMEZONE ?? 'Asia/Jerusalem',
    calendarProvider: parseCalendarProvider(process.env.CALENDAR_PROVIDER),
    googleCalendarId: process.env.GOOGLE_CALENDAR_ID ?? 'primary',
    googleCalendarRefreshToken: process.env.GOOGLE_CALENDAR_REFRESH_TOKEN,
    googleCalendarIcsUrl: process.env.GOOGLE_CALENDAR_ICS_URL,
    microsoftGraphAccessToken: process.env.MICROSOFT_GRAPH_ACCESS_TOKEN,
    vadThreshold: parseFloatEnv('VAD_THRESHOLD', 0.5),
    vadSilenceTimeoutMs: parseIntEnv('VAD_SILENCE_TIMEOUT_MS', 1800),
    voiceDebug: process.env.VOICE_DEBUG !== '0',
    backendAec3Enabled: process.env.BACKEND_AEC3_ENABLED === '1',
    aecInitialDelayMs: parseIntEnv('AEC_INITIAL_DELAY_MS', 100),
  };
}

export function validateAppConfig(config: AppConfig): void {
  if (!config.googleAiApiKey) {
    throw new Error('GOOGLE_AI_API_KEY is required.');
  }
}
