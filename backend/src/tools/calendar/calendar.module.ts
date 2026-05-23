import { Module } from '@nestjs/common';
import { AppConfigService } from '../../config/app-config.service';
import { CALENDAR_PROVIDER } from '../../agent/agent.tokens';
import { CalendarProvider } from './calendar-provider.interface';
import { MockCalendarProvider } from './mock-calendar-provider';
import { GoogleCalendarProvider } from './google-calendar-provider';
import { MicrosoftCalendarProvider } from './microsoft-calendar-provider';
import { CalendarToolService } from './calendar-tool.service';
import { CalendarConfigStore } from './calendar-config.store';
import { CalendarSettingsController } from './calendar-settings.controller';
import { ToolsModule } from '../tools.module';
import { AppLoggerService } from '../../logging/logger.module';

@Module({
  imports: [ToolsModule],
  controllers: [CalendarSettingsController],
  providers: [
    CalendarConfigStore,
    {
      provide: CALENDAR_PROVIDER,
      useFactory: (
        config: AppConfigService,
        calendarConfigStore: CalendarConfigStore,
        logger: AppLoggerService,
      ): CalendarProvider | null => {
        switch (config.calendarProvider) {
          case 'mock':
            return new MockCalendarProvider();
          case 'google':
            return new GoogleCalendarProvider(config, calendarConfigStore, logger);
          case 'microsoft':
            return new MicrosoftCalendarProvider(config);
          case 'none':
          default:
            return null;
        }
      },
      inject: [AppConfigService, CalendarConfigStore, AppLoggerService],
    },
    CalendarToolService,
  ],
  exports: [CALENDAR_PROVIDER, CalendarToolService, CalendarConfigStore],
})
export class CalendarModule {}
