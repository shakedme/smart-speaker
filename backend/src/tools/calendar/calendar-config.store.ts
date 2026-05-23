import { Injectable } from '@nestjs/common';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
import { join } from 'path';

export interface CalendarConfigData {
  googleIcsUrl?: string;
}

@Injectable()
export class CalendarConfigStore {
  private readonly configPath = join(process.cwd(), 'data', 'calendar-config.json');

  getGoogleIcsUrl(): string | undefined {
    const url = this.read().googleIcsUrl?.trim();
    return url && url.length > 0 ? url : undefined;
  }

  setGoogleIcsUrl(url: string): void {
    const data = this.read();
    data.googleIcsUrl = url.trim();
    this.write(data);
  }

  isGoogleConnected(): boolean {
    return this.getGoogleIcsUrl() !== undefined;
  }

  private read(): CalendarConfigData {
    if (!existsSync(this.configPath)) {
      return {};
    }
    try {
      const raw = readFileSync(this.configPath, 'utf8');
      const parsed = JSON.parse(raw) as CalendarConfigData;
      return typeof parsed === 'object' && parsed !== null ? parsed : {};
    } catch {
      return {};
    }
  }

  private write(data: CalendarConfigData): void {
    const dir = join(process.cwd(), 'data');
    if (!existsSync(dir)) {
      mkdirSync(dir, { recursive: true });
    }
    writeFileSync(this.configPath, `${JSON.stringify(data, null, 2)}\n`, 'utf8');
  }
}
