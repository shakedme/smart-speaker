import axios from 'axios';
import ical, { VEvent } from 'node-ical';
import { CalendarEvent } from './calendar-types';
import { CalendarProviderError } from '../../errors/agent-errors';

const CACHE_TTL_MS = 30 * 1000;

interface IcsCacheEntry {
  fetchedAt: number;
  events: CalendarEvent[];
}

const icsCache = new Map<string, IcsCacheEntry>();

export interface FetchIcsOptions {
  /** When true, always re-download the ICS feed (recommended for voice queries). */
  bypassCache?: boolean;
}

export function isValidGoogleIcsUrl(url: string): boolean {
  try {
    const parsed = new URL(url);
    return (
      parsed.protocol === 'https:' &&
      parsed.hostname === 'calendar.google.com' &&
      parsed.pathname.includes('/calendar/ical/')
    );
  } catch {
    return false;
  }
}

function toIso(value: Date | string | undefined | null): string {
  if (!value) {
    return '';
  }
  if (value instanceof Date) {
    return value.toISOString();
  }
  return String(value);
}

function asString(value: unknown): string | undefined {
  if (typeof value === 'string') {
    return value;
  }
  if (value && typeof value === 'object' && 'val' in value && typeof (value as { val: unknown }).val === 'string') {
    return (value as { val: string }).val;
  }
  return undefined;
}

function mapVEvent(event: VEvent): CalendarEvent | null {
  const title = asString(event.summary);
  if (!title || !event.start) {
    return null;
  }

  const isAllDay = event.datetype === 'date';
  const start = toIso(event.start);
  const end = toIso(event.end ?? event.start);
  if (!start) {
    return null;
  }

  const locationRaw = asString(event.location);
  const location = locationRaw && locationRaw.trim().length > 0 ? locationRaw.trim() : undefined;

  return {
    ...(event.uid ? { id: asString(event.uid) ?? String(event.uid) } : {}),
    title,
    start,
    end,
    isAllDay,
    ...(location ? { location } : {}),
    ...(event.status ? { status: asString(event.status) ?? String(event.status) } : {}),
  };
}

export function parseIcsEvents(icsContent: string): CalendarEvent[] {
  const parsed = ical.sync.parseICS(icsContent);
  const events: CalendarEvent[] = [];

  for (const item of Object.values(parsed)) {
    if (!item || item.type !== 'VEVENT') {
      continue;
    }
    const mapped = mapVEvent(item as VEvent);
    if (mapped) {
      events.push(mapped);
    }
  }

  return events;
}

async function fetchAllIcsEvents(icsUrl: string, options: FetchIcsOptions = {}): Promise<{
  events: CalendarEvent[];
  cacheStatus: 'hit' | 'miss';
}> {
  const cached = icsCache.get(icsUrl);
  const now = Date.now();
  if (!options.bypassCache && cached && now - cached.fetchedAt < CACHE_TTL_MS) {
    return { events: cached.events, cacheStatus: 'hit' };
  }

  let response;
  try {
    response = await axios.get<string>(icsUrl, {
      responseType: 'text',
      timeout: 15_000,
      headers: {
        Accept: 'text/calendar',
        'Cache-Control': 'no-cache',
        Pragma: 'no-cache',
        'User-Agent': 'Mozilla/5.0 (compatible; BTSpeakerCalendar/1.0)',
      },
      maxRedirects: 5,
    });
  } catch (error) {
    throw new CalendarProviderError(error);
  }

  if (typeof response.data !== 'string' || response.data.trim().length === 0) {
    throw new CalendarProviderError('ICS feed returned empty content');
  }

  const events = parseIcsEvents(response.data);
  icsCache.set(icsUrl, { fetchedAt: now, events });
  return { events, cacheStatus: 'miss' };
}

function eventOverlapsWindow(event: CalendarEvent, windowStartMs: number, windowEndMs: number): boolean {
  const startMs = new Date(event.start).getTime();
  const endMs = new Date(event.end).getTime();

  if (Number.isNaN(startMs) || Number.isNaN(endMs)) {
    return false;
  }

  if (event.isAllDay) {
    return startMs <= windowEndMs && endMs >= windowStartMs;
  }

  return startMs >= windowStartMs && startMs <= windowEndMs;
}

export interface FetchIcsEventsResult {
  events: CalendarEvent[];
  feedEventCount: number;
  cacheStatus: 'hit' | 'miss';
}

export async function fetchIcsEvents(
  icsUrl: string,
  args: {
    startTime: string;
    endTime: string;
    timezone: string;
    maxResults?: number;
  },
  options: FetchIcsOptions = { bypassCache: true },
): Promise<FetchIcsEventsResult> {
  const { events: allEvents, cacheStatus } = await fetchAllIcsEvents(icsUrl, options);
  const windowStartMs = new Date(args.startTime).getTime();
  const windowEndMs = new Date(args.endTime).getTime();

  if (Number.isNaN(windowStartMs) || Number.isNaN(windowEndMs)) {
    throw new CalendarProviderError('Invalid time window for calendar query');
  }

  const filtered = allEvents
    .filter((event) => eventOverlapsWindow(event, windowStartMs, windowEndMs))
    .sort((a, b) => new Date(a.start).getTime() - new Date(b.start).getTime());

  const limit = args.maxResults ?? 20;
  return {
    events: filtered.slice(0, limit),
    feedEventCount: allEvents.length,
    cacheStatus,
  };
}

/** Clear in-memory ICS cache (useful for tests). */
export function clearIcsCache(): void {
  icsCache.clear();
}
