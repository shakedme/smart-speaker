import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  resolveRelativeDate,
  resolveAgendaScope,
  getDayBounds,
  resolveSemanticTimeWindow,
} from './calendar-date-utils';

describe('calendar-date-utils', () => {
  const timezone = 'Asia/Jerusalem';
  const fixedNow = new Date('2026-05-23T10:00:00Z'); // Saturday in Israel

  it('resolveRelativeDate today', () => {
    const result = resolveRelativeDate('today', timezone, fixedNow);
    assert.match(result, /^\d{4}-\d{2}-\d{2}$/);
  });

  it('resolveRelativeDate tomorrow differs from today', () => {
    const today = resolveRelativeDate('today', timezone, fixedNow);
    const tomorrow = resolveRelativeDate('tomorrow', timezone, fixedNow);
    assert.notEqual(today, tomorrow);
  });

  it('resolveRelativeDate monday from Saturday resolves to upcoming Monday', () => {
    const monday = resolveRelativeDate('monday', timezone, fixedNow);
    assert.equal(monday, '2026-05-25');
  });

  it('resolveRelativeDate next monday skips upcoming Monday when today is Monday', () => {
    const mondayNow = new Date('2026-05-25T10:00:00Z');
    const nextMonday = resolveRelativeDate('next monday', timezone, mondayNow);
    assert.equal(nextMonday, '2026-06-01');
  });

  it('resolveRelativeDate monday on Monday returns same day', () => {
    const mondayNow = new Date('2026-05-25T10:00:00Z');
    const monday = resolveRelativeDate('monday', timezone, mondayNow);
    assert.equal(monday, '2026-05-25');
  });

  it('resolveAgendaScope next week from Saturday spans Mon-Sun', () => {
    const scope = resolveAgendaScope('next week', timezone, fixedNow);
    assert.equal(scope.kind, 'week');
    if (scope.kind === 'week') {
      assert.equal(scope.startDate, '2026-05-25');
      assert.equal(scope.endDate, '2026-05-31');
      assert.equal(scope.label, 'next week');
    }
  });

  it('resolveAgendaScope this week from Saturday includes remaining days', () => {
    const scope = resolveAgendaScope('this week', timezone, fixedNow);
    assert.equal(scope.kind, 'week');
    if (scope.kind === 'week') {
      assert.equal(scope.startDate, '2026-05-18');
      assert.equal(scope.endDate, '2026-05-24');
    }
  });

  it('getDayBounds returns ISO strings', () => {
    const date = '2026-05-23';
    const bounds = getDayBounds(date, timezone);
    assert.ok(bounds.startTime.includes('2026-05-23'));
    assert.ok(bounds.endTime.includes('2026-05-23'));
  });

  it('resolveSemanticTimeWindow afternoon', () => {
    const window = resolveSemanticTimeWindow(
      {
        startTime: 'afternoon_start',
        endTime: 'afternoon_end',
        timezone,
      },
      fixedNow,
    );
    assert.ok(window.startTime.length > 0);
    assert.ok(window.endTime.length > 0);
  });
});
