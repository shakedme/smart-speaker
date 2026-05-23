import { CalendarProvider } from './calendar-provider.interface';
import { CalendarEvent } from './calendar-types';
import { AppConfigService } from '../../config/app-config.service';
import { CalendarNotConnectedError } from '../../errors/agent-errors';

export class MicrosoftCalendarProvider implements CalendarProvider {
  constructor(private readonly config: AppConfigService) {}

  async getEvents(_args: {
    startTime: string;
    endTime: string;
    timezone: string;
    maxResults?: number;
  }): Promise<CalendarEvent[]> {
    if (!this.config.microsoftGraphAccessToken) {
      throw new CalendarNotConnectedError();
    }
    throw new CalendarNotConnectedError();
  }
}
