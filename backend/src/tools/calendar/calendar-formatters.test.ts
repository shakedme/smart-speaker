import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  formatDailyAgendaForSpeech,
  formatNextEventForSpeech,
  formatCalendarEventsForSpeech,
} from './calendar-formatters';

describe('calendar-formatters', () => {
  it('formatDailyAgendaForSpeech empty', () => {
    const text = formatDailyAgendaForSpeech({
      date: '2026-05-23',
      timezone: 'Asia/Jerusalem',
      eventCount: 0,
      events: [],
    });
    assert.ok(text.includes('no events'));
  });

  it('formatDailyAgendaForSpeech uses weekday label for future date', () => {
    const text = formatDailyAgendaForSpeech({
      date: '2026-05-25',
      timezone: 'Asia/Jerusalem',
      eventCount: 1,
      events: [
        {
          title: 'Standup',
          start: '2026-05-25T09:00:00+03:00',
          end: '2026-05-25T09:15:00+03:00',
          isAllDay: false,
        },
      ],
    });
    assert.ok(text.includes('Monday'));
    assert.ok(!text.includes(' today'));
  });

  it('formatDailyAgendaForSpeech weekly summary', () => {
    const text = formatDailyAgendaForSpeech({
      date: '2026-05-25',
      timezone: 'Asia/Jerusalem',
      eventCount: 2,
      period: 'week',
      startDate: '2026-05-25',
      endDate: '2026-05-31',
      periodLabel: 'next week',
      events: [
        {
          title: 'Standup',
          start: '2026-05-26T09:00:00+03:00',
          end: '2026-05-26T09:15:00+03:00',
          isAllDay: false,
        },
        {
          title: 'Review',
          start: '2026-05-28T14:00:00+03:00',
          end: '2026-05-28T15:00:00+03:00',
          isAllDay: false,
        },
      ],
    });
    assert.ok(text.includes('next week'));
    assert.ok(text.includes('Standup'));
    assert.ok(text.includes('Review'));
  });

  it('formatDailyAgendaForSpeech multiple events', () => {
    const text = formatDailyAgendaForSpeech({
      date: '2026-05-23',
      timezone: 'Asia/Jerusalem',
      eventCount: 3,
      events: [
        {
          title: 'Team Sync',
          start: '2026-05-23T09:30:00+03:00',
          end: '2026-05-23T10:00:00+03:00',
          isAllDay: false,
        },
        {
          title: 'Design Review',
          start: '2026-05-23T11:00:00+03:00',
          end: '2026-05-23T11:45:00+03:00',
          isAllDay: false,
        },
        {
          title: 'Alex 1:1',
          start: '2026-05-23T16:00:00+03:00',
          end: '2026-05-23T16:30:00+03:00',
          isAllDay: false,
        },
      ],
    });
    assert.ok(text.includes('3 events'));
    assert.ok(text.includes('Team Sync'));
  });

  it('formatNextEventForSpeech null', () => {
    const text = formatNextEventForSpeech({ event: null });
    assert.ok(text.includes('no upcoming'));
  });

  it('formatCalendarEventsForSpeech free', () => {
    const text = formatCalendarEventsForSpeech({ timezone: 'Asia/Jerusalem', events: [] });
    assert.ok(text.includes('free'));
  });
});
