import { Inject, Injectable, OnModuleInit, Optional } from '@nestjs/common';
import { AgentTool, ToolExecutionContext } from '../../agent/agent-types';
import { ToolRegistryService } from '../../agent/tool-registry.service';
import { CALENDAR_PROVIDER } from '../../agent/agent.tokens';
import { CalendarProvider } from './calendar-provider.interface';
import {
  GetCalendarEventsArgs,
  GetCalendarEventsResult,
  GetDailyAgendaArgs,
  GetDailyAgendaResult,
  GetNextCalendarEventArgs,
  GetNextCalendarEventResult,
} from './calendar-types';
import {
  getDayBounds,
  getLookaheadWindow,
  resolveAgendaScope,
  resolveSemanticTimeWindow,
} from './calendar-date-utils';
import { CalendarNotConnectedError, CalendarProviderError } from '../../errors/agent-errors';
import { AppLoggerService } from '../../logging/logger.module';

function asString(value: unknown, fallback = ''): string {
  return typeof value === 'string' ? value : fallback;
}

function asNumber(value: unknown, fallback: number): number {
  return typeof value === 'number' && !Number.isNaN(value) ? value : fallback;
}

@Injectable()
export class CalendarToolService implements OnModuleInit {
  constructor(
    private readonly toolRegistry: ToolRegistryService,
    @Optional() @Inject(CALENDAR_PROVIDER) private readonly provider: CalendarProvider | null,
    private readonly logger: AppLoggerService,
  ) {}

  onModuleInit(): void {
    this.toolRegistry.register(this.createDailyAgendaTool());
    this.toolRegistry.register(this.createNextEventTool());
    this.toolRegistry.register(this.createCalendarEventsTool());
  }

  private requireProvider(): CalendarProvider {
    if (!this.provider) {
      throw new CalendarNotConnectedError();
    }
    return this.provider;
  }

  private createDailyAgendaTool(): AgentTool<GetDailyAgendaArgs, GetDailyAgendaResult> {
    return {
      name: 'get_daily_agenda',
      description: 'Get all calendar events for a specific day or week',
      execute: async (args, context) => {
        const provider = this.requireProvider();
        const timezone = args.timezone || context.timezone;
        const scope = resolveAgendaScope(args.date, timezone, context.now);
        const bounds =
          scope.kind === 'week'
            ? {
                startTime: getDayBounds(scope.startDate, timezone).startTime,
                endTime: getDayBounds(scope.endDate, timezone).endTime,
              }
            : getDayBounds(scope.date, timezone);
        const logRange =
          scope.kind === 'week' ? `${scope.startDate}..${scope.endDate}` : scope.date;
        this.logger.log('TOOL', 'info', `execute name=get_daily_agenda range=${logRange}`);

        try {
          const events = await provider.getEvents({
            startTime: bounds.startTime,
            endTime: bounds.endTime,
            timezone,
            maxResults: args.maxResults ?? (scope.kind === 'week' ? 50 : 20),
          });
          const filtered = args.includeAllDay === false ? events.filter((e) => !e.isAllDay) : events;
          this.logger.log('CALENDAR', 'info', `agenda range=${logRange} events=${filtered.length}`);
          if (scope.kind === 'week') {
            return {
              date: scope.startDate,
              timezone,
              eventCount: filtered.length,
              events: filtered,
              period: 'week',
              startDate: scope.startDate,
              endDate: scope.endDate,
              periodLabel: scope.label,
            };
          }
          return { date: scope.date, timezone, eventCount: filtered.length, events: filtered, period: 'day' };
        } catch (error) {
          if (error instanceof CalendarNotConnectedError) {
            throw error;
          }
          throw new CalendarProviderError(error);
        }
      },
    };
  }

  private createNextEventTool(): AgentTool<GetNextCalendarEventArgs, GetNextCalendarEventResult> {
    return {
      name: 'get_next_calendar_event',
      description: 'Get the next upcoming calendar event',
      execute: async (args, context) => {
        const provider = this.requireProvider();
        const timezone = args.timezone || context.timezone;
        const hours = args.lookaheadHours ?? 48;
        const window = getLookaheadWindow(hours, timezone, context.now);
        this.logger.log('TOOL', 'info', `execute name=get_next_calendar_event`);

        try {
          const events = await provider.getEvents({
            startTime: window.startTime,
            endTime: window.endTime,
            timezone,
            maxResults: 10,
          });
          const nowMs = (context.now ?? new Date()).getTime();
          const upcoming = events
            .filter((e) => !e.isAllDay || new Date(e.start).getTime() >= nowMs)
            .sort((a, b) => new Date(a.start).getTime() - new Date(b.start).getTime());
          const event = upcoming.find((e) => new Date(e.start).getTime() >= nowMs) ?? upcoming[0] ?? null;
          return { event: event ?? null };
        } catch (error) {
          if (error instanceof CalendarNotConnectedError) {
            throw error;
          }
          throw new CalendarProviderError(error);
        }
      },
    };
  }

  private createCalendarEventsTool(): AgentTool<GetCalendarEventsArgs, GetCalendarEventsResult> {
    return {
      name: 'get_calendar_events',
      description: 'Get calendar events within a time window',
      execute: async (args, context) => {
        const provider = this.requireProvider();
        const timezone = args.timezone || context.timezone;
        const window = resolveSemanticTimeWindow(
          {
            startTime: args.startTime,
            endTime: args.endTime,
            timezone,
          },
          context.now,
        );
        this.logger.log('TOOL', 'info', `execute name=get_calendar_events`);

        try {
          const events = await provider.getEvents({
            startTime: window.startTime,
            endTime: window.endTime,
            timezone,
            maxResults: args.maxResults ?? 10,
          });
          return { timezone, events };
        } catch (error) {
          if (error instanceof CalendarNotConnectedError) {
            throw error;
          }
          throw new CalendarProviderError(error);
        }
      },
    };
  }

  /** Execute tool from raw args record (from intent classifier). */
  async executeRaw(
    toolName: string,
    args: Record<string, unknown>,
    context: ToolExecutionContext,
  ): Promise<unknown> {
    const normalizedArgs = {
      ...args,
      date: asString(args.date, 'today'),
      timezone: asString(args.timezone, context.timezone),
      startTime: asString(args.startTime),
      endTime: asString(args.endTime),
      lookaheadHours: asNumber(args.lookaheadHours, 48),
      maxResults: asNumber(args.maxResults, 20),
    };
    return this.toolRegistry.execute(toolName, normalizedArgs, context);
  }
}
