import { Injectable } from '@nestjs/common';
import { ConfigService } from '@nestjs/config';
import { AppConfig } from './env.schema';

@Injectable()
export class AppConfigService {
  constructor(private readonly configService: ConfigService<AppConfig, true>) {}

  get port(): number {
    return this.configService.get('port', { infer: true });
  }

  get nodeEnv(): string {
    return this.configService.get('nodeEnv', { infer: true });
  }

  get googleAiApiKey(): string {
    return this.configService.get('googleAiApiKey', { infer: true });
  }

  get spotifyClientId(): string {
    return this.configService.get('spotifyClientId', { infer: true });
  }

  get spotifyClientSecret(): string {
    return this.configService.get('spotifyClientSecret', { infer: true });
  }

  get userTimezone(): string {
    return this.configService.get('userTimezone', { infer: true });
  }

  get calendarProvider(): AppConfig['calendarProvider'] {
    return this.configService.get('calendarProvider', { infer: true });
  }

  get googleCalendarId(): string {
    return this.configService.get('googleCalendarId', { infer: true });
  }

  get googleCalendarRefreshToken(): string | undefined {
    return this.configService.get('googleCalendarRefreshToken', { infer: true });
  }

  get googleCalendarIcsUrl(): string | undefined {
    return this.configService.get('googleCalendarIcsUrl', { infer: true });
  }

  get microsoftGraphAccessToken(): string | undefined {
    return this.configService.get('microsoftGraphAccessToken', { infer: true });
  }

  get vadThreshold(): number {
    return this.configService.get('vadThreshold', { infer: true });
  }

  get vadSilenceTimeoutMs(): number {
    return this.configService.get('vadSilenceTimeoutMs', { infer: true });
  }

  get voiceDebug(): boolean {
    return this.configService.get('voiceDebug', { infer: true });
  }

  get backendAec3Enabled(): boolean {
    return this.configService.get('backendAec3Enabled', { infer: true });
  }

  get aecInitialDelayMs(): number {
    return this.configService.get('aecInitialDelayMs', { infer: true });
  }
}
