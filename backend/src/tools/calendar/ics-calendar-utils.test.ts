import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import {
  parseIcsEvents,
  isValidGoogleIcsUrl,
} from './ics-calendar-utils';

const SAMPLE_ICS = `BEGIN:VCALENDAR
VERSION:2.0
BEGIN:VEVENT
UID:test-1
SUMMARY:Team Sync
DTSTART:20260525T093000Z
DTEND:20260525T100000Z
LOCATION:Zoom
END:VEVENT
BEGIN:VEVENT
UID:test-2
SUMMARY:Holiday
DTSTART;VALUE=DATE:20260526
DTEND;VALUE=DATE:20260527
END:VEVENT
END:VCALENDAR`;

describe('ics-calendar-utils', () => {
  it('isValidGoogleIcsUrl accepts Google secret iCal URLs', () => {
    assert.ok(
      isValidGoogleIcsUrl(
        'https://calendar.google.com/calendar/ical/user%40gmail.com/private-abc123/basic.ics',
      ),
    );
    assert.equal(isValidGoogleIcsUrl('https://example.com/calendar.ics'), false);
  });

  it('parseIcsEvents maps VEVENT fields', () => {
    const events = parseIcsEvents(SAMPLE_ICS);
    assert.equal(events.length, 2);

    const timed = events.find((e) => e.title === 'Team Sync');
    assert.ok(timed);
    assert.equal(timed!.isAllDay, false);
    assert.equal(timed!.location, 'Zoom');

    const allDay = events.find((e) => e.title === 'Holiday');
    assert.ok(allDay);
    assert.equal(allDay!.isAllDay, true);
  });
});
