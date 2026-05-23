export interface CalendarEvent {
  id?: string;
  title: string;
  start: string;
  end: string;
  isAllDay: boolean;
  location?: string;
  status?: string;
}

export interface GetDailyAgendaArgs {
  date: string;
  timezone: string;
  includeAllDay?: boolean;
  maxResults?: number;
}

export interface GetDailyAgendaResult {
  date: string;
  timezone: string;
  eventCount: number;
  events: CalendarEvent[];
  period?: 'day' | 'week';
  startDate?: string;
  endDate?: string;
  periodLabel?: string;
}

export interface GetNextCalendarEventArgs {
  timezone: string;
  lookaheadHours?: number;
}

export interface GetNextCalendarEventResult {
  event: CalendarEvent | null;
}

export interface GetCalendarEventsArgs {
  startTime: string;
  endTime: string;
  timezone: string;
  maxResults?: number;
}

export interface GetCalendarEventsResult {
  timezone: string;
  events: CalendarEvent[];
}

export interface TimeWindow {
  startTime: string;
  endTime: string;
}
