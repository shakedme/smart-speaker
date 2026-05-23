/** Pure date/time helpers using Intl — no external deps. */

const WEEKDAY_NAMES = [
  'sunday',
  'monday',
  'tuesday',
  'wednesday',
  'thursday',
  'friday',
  'saturday',
] as const;

function zonedParts(date: Date, timezone: string): { year: number; month: number; day: number } {
  const formatter = new Intl.DateTimeFormat('en-US', {
    timeZone: timezone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
  });
  const parts = formatter.formatToParts(date);
  const year = parseInt(parts.find((p) => p.type === 'year')?.value ?? '1970', 10);
  const month = parseInt(parts.find((p) => p.type === 'month')?.value ?? '1', 10);
  const day = parseInt(parts.find((p) => p.type === 'day')?.value ?? '1', 10);
  return { year, month, day };
}

function formatYmd(year: number, month: number, day: number): string {
  return `${year}-${String(month).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
}

function addDaysYmd(year: number, month: number, day: number, delta: number): string {
  const d = new Date(Date.UTC(year, month - 1, day + delta));
  return formatYmd(d.getUTCFullYear(), d.getUTCMonth() + 1, d.getUTCDate());
}

function offsetForTimezone(date: Date, timezone: string): string {
  const formatter = new Intl.DateTimeFormat('en-US', {
    timeZone: timezone,
    timeZoneName: 'shortOffset',
  });
  const parts = formatter.formatToParts(date);
  const tzName = parts.find((p) => p.type === 'timeZoneName')?.value ?? 'GMT';
  const match = tzName.match(/GMT([+-]\d{1,2})(?::(\d{2}))?/);
  if (!match) {
    return '+00:00';
  }
  const sign = match[1]!.startsWith('-') ? '-' : '+';
  const hours = Math.abs(parseInt(match[1]!, 10)).toString().padStart(2, '0');
  const minutes = match[2] ?? '00';
  return `${sign}${hours}:${minutes}`;
}

function toIsoInTimezone(ymd: string, hour: number, minute: number, timezone: string): string {
  const utcGuess = new Date(`${ymd}T${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}:00Z`);
  const offset = offsetForTimezone(utcGuess, timezone);
  return `${ymd}T${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}:00${offset}`;
}

function dayOfWeekInTimezone(ymd: string, timezone: string): number {
  const weekday = new Intl.DateTimeFormat('en-US', {
    timeZone: timezone,
    weekday: 'long',
  })
    .format(new Date(`${ymd}T12:00:00Z`))
    .toLowerCase();
  return WEEKDAY_NAMES.indexOf(weekday as (typeof WEEKDAY_NAMES)[number]);
}

function resolveWeekdayDate(
  dayName: string,
  modifier: 'next' | 'upcoming',
  timezone: string,
  now: Date,
): string | null {
  const targetDow = WEEKDAY_NAMES.indexOf(dayName as (typeof WEEKDAY_NAMES)[number]);
  if (targetDow < 0) {
    return null;
  }

  const { year, month, day } = zonedParts(now, timezone);
  const todayYmd = formatYmd(year, month, day);
  const currentDow = dayOfWeekInTimezone(todayYmd, timezone);
  if (currentDow < 0) {
    return null;
  }

  let delta = (targetDow - currentDow + 7) % 7;
  if (modifier === 'next') {
    delta = delta === 0 ? 7 : delta;
  }

  return addDaysYmd(year, month, day, delta);
}

function parseWeekdayInput(normalized: string): { dayName: string; modifier: 'next' | 'upcoming' } | null {
  const nextMatch = normalized.match(
    /^next\s+(sunday|monday|tuesday|wednesday|thursday|friday|saturday)$/,
  );
  if (nextMatch) {
    return { dayName: nextMatch[1]!, modifier: 'next' };
  }

  const thisMatch = normalized.match(
    /^this\s+(sunday|monday|tuesday|wednesday|thursday|friday|saturday)$/,
  );
  if (thisMatch) {
    return { dayName: thisMatch[1]!, modifier: 'upcoming' };
  }

  if ((WEEKDAY_NAMES as readonly string[]).includes(normalized)) {
    return { dayName: normalized, modifier: 'upcoming' };
  }

  return null;
}

export type AgendaScope =
  | { kind: 'day'; date: string }
  | { kind: 'week'; startDate: string; endDate: string; label: 'this week' | 'next week' };

function addDaysFromYmd(ymd: string, delta: number): string {
  const [year, month, day] = ymd.split('-').map((part) => parseInt(part, 10));
  return addDaysYmd(year!, month!, day!, delta);
}

function resolveWeekScope(
  weekOffset: 0 | 1,
  timezone: string,
  now: Date,
): Extract<AgendaScope, { kind: 'week' }> {
  const { year, month, day } = zonedParts(now, timezone);
  const todayYmd = formatYmd(year, month, day);
  const dow = dayOfWeekInTimezone(todayYmd, timezone);
  const daysSinceMonday = dow === 0 ? 6 : dow - 1;
  const thisWeekMonday = addDaysYmd(year, month, day, -daysSinceMonday);
  const weekMonday = addDaysFromYmd(thisWeekMonday, weekOffset * 7);
  const weekSunday = addDaysFromYmd(weekMonday, 6);

  return {
    kind: 'week',
    startDate: weekMonday,
    endDate: weekSunday,
    label: weekOffset === 0 ? 'this week' : 'next week',
  };
}

function parseWeekInput(normalized: string): 0 | 1 | null {
  if (normalized === 'this week' || normalized === 'the week') {
    return 0;
  }
  if (normalized === 'next week' || normalized === 'the upcoming week' || normalized === 'upcoming week') {
    return 1;
  }
  return null;
}

/** Resolve a day, week, or weekday agenda query in the given timezone. */
export function resolveAgendaScope(input: string, timezone: string, now: Date = new Date()): AgendaScope {
  const normalized = input.trim().toLowerCase();
  const weekOffset = parseWeekInput(normalized);
  if (weekOffset !== null) {
    return resolveWeekScope(weekOffset, timezone, now);
  }
  return { kind: 'day', date: resolveRelativeDate(input, timezone, now) };
}

/** Resolve "today", "tomorrow", weekday names, or YYYY-MM-DD in the given timezone. */
export function resolveRelativeDate(input: string, timezone: string, now: Date = new Date()): string {
  const normalized = input.trim().toLowerCase();
  const weekOffset = parseWeekInput(normalized);
  if (weekOffset !== null) {
    return resolveWeekScope(weekOffset, timezone, now).startDate;
  }

  const { year, month, day } = zonedParts(now, timezone);
  const today = formatYmd(year, month, day);

  if (normalized === 'today') {
    return today;
  }
  if (normalized === 'tomorrow') {
    return addDaysYmd(year, month, day, 1);
  }
  if (/^\d{4}-\d{2}-\d{2}$/.test(input.trim())) {
    return input.trim();
  }

  const weekday = parseWeekdayInput(normalized);
  if (weekday) {
    const resolved = resolveWeekdayDate(weekday.dayName, weekday.modifier, timezone, now);
    if (resolved) {
      return resolved;
    }
  }

  return today;
}

export function getDayBounds(date: string, timezone: string): { startTime: string; endTime: string } {
  return {
    startTime: toIsoInTimezone(date, 0, 0, timezone),
    endTime: toIsoInTimezone(date, 23, 59, timezone),
  };
}

export function getAfternoonBounds(date: string, timezone: string): { startTime: string; endTime: string } {
  return {
    startTime: toIsoInTimezone(date, 12, 0, timezone),
    endTime: toIsoInTimezone(date, 17, 59, timezone),
  };
}

export function getAfterLunchBounds(date: string, timezone: string): { startTime: string; endTime: string } {
  const dayEnd = getDayBounds(date, timezone);
  return {
    startTime: toIsoInTimezone(date, 13, 0, timezone),
    endTime: dayEnd.endTime,
  };
}

export function getLookaheadWindow(
  hours: number,
  timezone: string,
  now: Date = new Date(),
): { startTime: string; endTime: string } {
  const end = new Date(now.getTime() + hours * 60 * 60 * 1000);
  const { year: sy, month: sm, day: sd } = zonedParts(now, timezone);
  const { year: ey, month: em, day: ed } = zonedParts(end, timezone);
  const startYmd = formatYmd(sy, sm, sd);
  const endYmd = formatYmd(ey, em, ed);

  const startHour = parseInt(
    now.toLocaleString('en-US', { timeZone: timezone, hour: 'numeric', hour12: false }),
    10,
  );
  const startMinute = parseInt(
    now.toLocaleString('en-US', { timeZone: timezone, minute: 'numeric' }),
    10,
  );
  const endHour = parseInt(
    end.toLocaleString('en-US', { timeZone: timezone, hour: 'numeric', hour12: false }),
    10,
  );
  const endMinute = parseInt(
    end.toLocaleString('en-US', { timeZone: timezone, minute: 'numeric' }),
    10,
  );

  return {
    startTime: toIsoInTimezone(startYmd, startHour, startMinute, timezone),
    endTime: toIsoInTimezone(endYmd, endHour, endMinute, timezone),
  };
}

export function resolveSemanticTimeWindow(
  args: { startTime: string; endTime: string; timezone: string; date?: string },
  now: Date = new Date(),
): { startTime: string; endTime: string } {
  const date = resolveRelativeDate(args.date ?? 'today', args.timezone, now);
  const start = args.startTime.trim().toLowerCase();
  const end = args.endTime.trim().toLowerCase();

  if (start === 'after_lunch' && end === 'end_of_day') {
    return getAfterLunchBounds(date, args.timezone);
  }
  if (start === 'afternoon_start' && end === 'afternoon_end') {
    return getAfternoonBounds(date, args.timezone);
  }
  if (/^\d{1,2}:\d{2}$/.test(start) && /^\d{1,2}:\d{2}$/.test(end)) {
    const [sh, sm] = start.split(':').map((v) => parseInt(v, 10));
    const [eh, em] = end.split(':').map((v) => parseInt(v, 10));
    return {
      startTime: toIsoInTimezone(date, sh!, sm!, args.timezone),
      endTime: toIsoInTimezone(date, eh!, em!, args.timezone),
    };
  }

  return getDayBounds(date, args.timezone);
}
