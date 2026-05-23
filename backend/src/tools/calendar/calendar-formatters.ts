import {
  CalendarEvent,
  GetCalendarEventsResult,
  GetDailyAgendaResult,
  GetNextCalendarEventResult,
} from './calendar-types';
import { resolveRelativeDate } from './calendar-date-utils';

function formatSpokenTime(iso: string, isAllDay: boolean): string {
  if (isAllDay) {
    return 'all day';
  }
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) {
    return iso;
  }
  return date.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', hour12: true });
}

function formatSpokenDate(iso: string): string {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) {
    return iso;
  }
  return date.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric' });
}

function formatAgendaDayLabel(date: string, timezone: string, now: Date = new Date()): string {
  const today = resolveRelativeDate('today', timezone, now);
  const tomorrow = resolveRelativeDate('tomorrow', timezone, now);
  if (date === today) {
    return 'today';
  }
  if (date === tomorrow) {
    return 'tomorrow';
  }
  return `on ${formatSpokenDate(`${date}T12:00:00Z`)}`;
}

function formatAgendaPeriodLabel(result: GetDailyAgendaResult, now: Date = new Date()): string {
  if (result.period === 'week') {
    return result.periodLabel ?? 'this week';
  }
  return formatAgendaDayLabel(result.date, result.timezone, now);
}

function formatWeeklyAgendaForSpeech(result: GetDailyAgendaResult, periodLabel: string): string {
  const parts: string[] = [`You have ${result.eventCount} events ${periodLabel}.`];
  const summaries = result.events.slice(0, 4).map((event) => {
    const day = formatSpokenDate(event.start);
    const time = formatSpokenTime(event.start, event.isAllDay);
    return `${event.title} on ${day} at ${time}`;
  });
  if (summaries.length > 0) {
    parts.push(`${summaries.join('. ')}${result.events.length > 4 ? ', and others.' : '.'}`);
  }
  return parts.join(' ');
}

export function formatDailyAgendaForSpeech(result: GetDailyAgendaResult): string {
  const periodLabel = formatAgendaPeriodLabel(result);

  if (result.eventCount === 0) {
    return `You have no events on your calendar ${periodLabel}.`;
  }

  if (result.period === 'week') {
    return formatWeeklyAgendaForSpeech(result, periodLabel);
  }

  const dayLabel = periodLabel;

  const timedEvents = result.events.filter((e) => !e.isAllDay);
  const allDayEvents = result.events.filter((e) => e.isAllDay);

  if (result.eventCount === 1 && result.events[0]) {
    const event = result.events[0];
    const time = formatSpokenTime(event.start, event.isAllDay);
    const location = event.location && event.location.length < 40 ? ` at ${event.location}` : '';
    return `You have one event ${dayLabel}: ${event.title} at ${time}${location}.`;
  }

  const parts: string[] = [`You have ${result.eventCount} events ${dayLabel}.`];

  if (allDayEvents.length > 0) {
    parts.push(
      allDayEvents.length === 1
        ? `${allDayEvents[0]!.title} is an all-day event.`
        : `You have ${allDayEvents.length} all-day events.`,
    );
  }

  if (timedEvents.length > 0) {
    const first = timedEvents[0]!;
    parts.push(`First is ${first.title} at ${formatSpokenTime(first.start, false)}.`);

    if (timedEvents.length > 2) {
      const middle = timedEvents[Math.floor(timedEvents.length / 2)]!;
      parts.push(`Then ${middle.title} at ${formatSpokenTime(middle.start, false)}.`);
    }

    if (timedEvents.length > 1) {
      const last = timedEvents[timedEvents.length - 1]!;
      parts.push(`Your last event is ${last.title} at ${formatSpokenTime(last.start, false)}.`);
    }
  }

  return parts.join(' ');
}

export function formatNextEventForSpeech(result: GetNextCalendarEventResult): string {
  if (!result.event) {
    return 'You have no upcoming meetings on your calendar.';
  }
  const event = result.event;
  const time = formatSpokenTime(event.start, event.isAllDay);
  const date = formatSpokenDate(event.start);
  const location = event.location && event.location.length < 40 ? ` at ${event.location}` : '';
  if (event.isAllDay) {
    return `Your next event is ${event.title}, an all-day event on ${date}.`;
  }
  return `Your next meeting is ${event.title} at ${time} on ${date}${location}.`;
}

export function formatCalendarEventsForSpeech(result: GetCalendarEventsResult): string {
  if (result.events.length === 0) {
    return 'You are free during that time.';
  }

  if (result.events.length === 1 && result.events[0]) {
    const event = result.events[0];
    return `You have ${event.title} at ${formatSpokenTime(event.start, event.isAllDay)}.`;
  }

  const summaries = result.events.slice(0, 3).map((event: CalendarEvent) => {
    return `${event.title} at ${formatSpokenTime(event.start, event.isAllDay)}`;
  });

  if (result.events.length > 3) {
    return `You have ${result.events.length} events: ${summaries.join(', ')}, and others.`;
  }
  return `You have ${result.events.length} events: ${summaries.join(', ')}.`;
}
