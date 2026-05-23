import { Body, Controller, Get, Post, Res } from '@nestjs/common';
import { Response } from 'express';
import { AppConfigService } from '../../config/app-config.service';
import { CalendarConfigStore } from './calendar-config.store';
import { isValidGoogleIcsUrl } from './ics-calendar-utils';

@Controller('calendar')
export class CalendarSettingsController {
  constructor(
    private readonly calendarConfigStore: CalendarConfigStore,
    private readonly config: AppConfigService,
  ) {}

  @Get('status')
  status(): { connected: boolean; provider: string } {
    const envUrl = this.config.googleCalendarIcsUrl;
    const connected =
      this.config.calendarProvider === 'google' &&
      (this.calendarConfigStore.isGoogleConnected() || Boolean(envUrl));
    return {
      connected,
      provider: this.config.calendarProvider,
    };
  }

  @Get('settings')
  settingsPage(@Res() res: Response): void {
    const connected = this.calendarConfigStore.isGoogleConnected();
    const html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Calendar Settings</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 640px; margin: 40px auto; padding: 0 20px; line-height: 1.5; }
    h1 { font-size: 1.5rem; }
    label { display: block; font-weight: 600; margin-bottom: 8px; }
    input[type=url] { width: 100%; padding: 10px; box-sizing: border-box; font-size: 14px; }
    button { margin-top: 16px; padding: 10px 18px; font-size: 15px; cursor: pointer; }
    .status { padding: 12px; border-radius: 8px; margin-bottom: 20px; }
    .ok { background: #e8f5e9; color: #1b5e20; }
    .warn { background: #fff8e1; color: #e65100; }
    ol { padding-left: 20px; }
    code { background: #f5f5f5; padding: 2px 4px; border-radius: 4px; }
  </style>
</head>
<body>
  <h1>Google Calendar Setup</h1>
  <div class="status ${connected ? 'ok' : 'warn'}">
    ${connected ? 'Calendar connected via saved iCal URL.' : 'Calendar not connected yet.'}
  </div>
  <p>Paste your <strong>Secret address in iCal format</strong> from Google Calendar:</p>
  <ol>
    <li>Open <a href="https://calendar.google.com" target="_blank" rel="noopener">Google Calendar</a></li>
    <li>Settings → select your calendar → Integrate calendar</li>
    <li>Copy the secret iCal URL (looks like <code>calendar.google.com/calendar/ical/.../basic.ics</code>)</li>
  </ol>
  <form method="post" action="/calendar/settings">
    <label for="googleIcsUrl">Secret iCal URL</label>
    <input id="googleIcsUrl" name="googleIcsUrl" type="url" required placeholder="https://calendar.google.com/calendar/ical/.../basic.ics">
    <button type="submit">Save</button>
  </form>
  <p style="margin-top:24px;color:#666;font-size:14px;">Also set <code>CALENDAR_PROVIDER=google</code> in your backend <code>.env</code> and restart the server.</p>
</body>
</html>`;
    res.type('html').send(html);
  }

  @Post('settings')
  saveSettings(@Body('googleIcsUrl') googleIcsUrl: string, @Res() res: Response): void {
    const url = typeof googleIcsUrl === 'string' ? googleIcsUrl.trim() : '';
    if (!isValidGoogleIcsUrl(url)) {
      res.status(400).type('html').send(this.errorPage('Invalid Google Calendar iCal URL.'));
      return;
    }

    this.calendarConfigStore.setGoogleIcsUrl(url);
    res.type('html').send(this.successPage());
  }

  private successPage(): string {
    return `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><title>Saved</title></head>
<body style="font-family:system-ui;max-width:640px;margin:40px auto;padding:0 20px;">
  <h1>Calendar saved</h1>
  <p>Your Google Calendar iCal URL was saved. Set <code>CALENDAR_PROVIDER=google</code> in <code>.env</code> if you have not already, then restart the backend.</p>
  <p><a href="/calendar/settings">Back to settings</a></p>
</body></html>`;
  }

  private errorPage(message: string): string {
    return `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><title>Error</title></head>
<body style="font-family:system-ui;max-width:640px;margin:40px auto;padding:0 20px;">
  <h1>Could not save</h1>
  <p>${message}</p>
  <p><a href="/calendar/settings">Try again</a></p>
</body></html>`;
  }
}
