import { CalendarProvider } from './calendar-provider.interface';
import { CalendarEvent } from './calendar-types';
import { AppConfigService } from '../../config/app-config.service';
import { CalendarConfigStore } from './calendar-config.store';
import { fetchIcsEvents } from './ics-calendar-utils';
import { CalendarNotConnectedError, CalendarProviderError } from '../../errors/agent-errors';
import { AppLoggerService } from '../../logging/logger.module';

export class GoogleCalendarProvider implements CalendarProvider {
  constructor(
    private readonly config: AppConfigService,
    private readonly calendarConfigStore: CalendarConfigStore,
    private readonly logger: AppLoggerService,
  ) {}

  private resolveIcsUrl(): string | undefined {
    return this.calendarConfigStore.getGoogleIcsUrl() ?? this.config.googleCalendarIcsUrl;
  }

  async getEvents(args: {
    startTime: string;
    endTime: string;
    timezone: string;
    maxResults?: number;
  }): Promise<CalendarEvent[]> {
    const icsUrl = this.resolveIcsUrl();
    if (!icsUrl) {
      throw new CalendarNotConnectedError();
    }

    try {
      const result = await fetchIcsEvents(icsUrl, args, { bypassCache: true });
      this.logger.log(
        'CALENDAR',
        'info',
        `ics feed=${result.feedEventCount} matched=${result.events.length} cache=${result.cacheStatus} window=${args.startTime}..${args.endTime}`,
      );
      return result.events;
    } catch (error) {
      if (error instanceof CalendarNotConnectedError) {
        throw error;
      }
      if (error instanceof CalendarProviderError) {
        throw error;
      }
      throw new CalendarProviderError(error);
    }
  }
}
