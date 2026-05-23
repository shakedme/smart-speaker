import { CalendarEvent } from './calendar-types';

export interface CalendarProvider {
  getEvents(args: {
    startTime: string;
    endTime: string;
    timezone: string;
    maxResults?: number;
  }): Promise<CalendarEvent[]>;
}
