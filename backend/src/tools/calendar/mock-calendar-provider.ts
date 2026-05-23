import { CalendarProvider } from './calendar-provider.interface';
import { CalendarEvent } from './calendar-types';
import { resolveRelativeDate, getDayBounds } from './calendar-date-utils';

function toIsoInTimezoneLocal(ymd: string, hour: number, minute: number, timezone: string): string {
  const formatter = new Intl.DateTimeFormat('en-US', {
    timeZone: timezone,
    timeZoneName: 'shortOffset',
  });
  const utcGuess = new Date(`${ymd}T${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}:00Z`);
  const parts = formatter.formatToParts(utcGuess);
  const tzName = parts.find((p) => p.type === 'timeZoneName')?.value ?? 'GMT';
  const match = tzName.match(/GMT([+-]\d{1,2})(?::(\d{2}))?/);
  const offset = match
    ? `${match[1]!.startsWith('-') ? '-' : '+'}${Math.abs(parseInt(match[1]!, 10)).toString().padStart(2, '0')}:${match[2] ?? '00'}`
    : '+00:00';
  return `${ymd}T${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}:00${offset}`;
}

function buildMockEventsForDate(date: string, timezone: string): CalendarEvent[] {
  return [
    {
      id: `mock-1-${date}`,
      title: 'Team Sync',
      start: toIsoInTimezoneLocal(date, 9, 30, timezone),
      end: toIsoInTimezoneLocal(date, 10, 0, timezone),
      isAllDay: false,
      location: 'Zoom',
    },
    {
      id: `mock-2-${date}`,
      title: 'Design Review',
      start: toIsoInTimezoneLocal(date, 11, 0, timezone),
      end: toIsoInTimezoneLocal(date, 11, 45, timezone),
      isAllDay: false,
    },
    {
      id: `mock-3-${date}`,
      title: 'Alex 1:1',
      start: toIsoInTimezoneLocal(date, 16, 0, timezone),
      end: toIsoInTimezoneLocal(date, 16, 30, timezone),
      isAllDay: false,
    },
    {
      id: `mock-allday-${date}`,
      title: 'Company Holiday',
      start: `${date}T00:00:00`,
      end: `${date}T23:59:59`,
      isAllDay: true,
    },
  ];
}

export class MockCalendarProvider implements CalendarProvider {
  async getEvents(args: {
    startTime: string;
    endTime: string;
    timezone: string;
    maxResults?: number;
  }): Promise<CalendarEvent[]> {
    const startDate = args.startTime.slice(0, 10);
    const today = resolveRelativeDate('today', args.timezone);
    const tomorrow = resolveRelativeDate('tomorrow', args.timezone);

    let events: CalendarEvent[] = [];
    if (startDate === today) {
      events = buildMockEventsForDate(today, args.timezone);
    } else if (startDate === tomorrow) {
      events = buildMockEventsForDate(tomorrow, args.timezone).slice(0, 2);
    } else {
      const bounds = getDayBounds(startDate, args.timezone);
      events = buildMockEventsForDate(startDate, args.timezone).filter((e) => {
        return e.start >= bounds.startTime && e.start <= bounds.endTime;
      });
    }

    const filtered = events.filter((e) => {
      if (e.isAllDay) {
        return true;
      }
      return e.start >= args.startTime && e.start <= args.endTime;
    });

    const limit = args.maxResults ?? 20;
    return filtered.slice(0, limit);
  }
}
