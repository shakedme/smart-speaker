import { Injectable } from '@nestjs/common';
import { ClassifiedIntent } from '../intent/intent-types';
import { AgentContext, AgentResult } from './agent-types';
import { ToolRegistryService } from './tool-registry.service';
import { SpotifyService } from '../tools/spotify/spotify.service';
import { CalendarToolService } from '../tools/calendar/calendar-tool.service';
import { AppLoggerService } from '../logging/logger.module';
import {
  AgentError,
  CalendarNotConnectedError,
  CalendarProviderError,
  SpotifyResolutionError,
  ToolNotFoundError,
} from '../errors/agent-errors';
import {
  formatCalendarEventsForSpeech,
  formatDailyAgendaForSpeech,
  formatNextEventForSpeech,
} from '../tools/calendar/calendar-formatters';
import {
  GetCalendarEventsResult,
  GetDailyAgendaResult,
  GetNextCalendarEventResult,
} from '../tools/calendar/calendar-types';

@Injectable()
export class AgentOrchestratorService {
  constructor(
    private readonly spotifyService: SpotifyService,
    private readonly calendarToolService: CalendarToolService,
    private readonly toolRegistry: ToolRegistryService,
    private readonly logger: AppLoggerService,
  ) {}

  async handleIntent(intent: ClassifiedIntent, context: AgentContext): Promise<AgentResult> {
    try {
      switch (intent.intent) {
        case 'spotify':
          return this.handleSpotify(intent.data);
        case 'playback_control':
          return {
            type: 'device_command',
            command: {
              type: 'intent',
              intent: 'playback_control',
              data: intent.data,
            },
          };
        case 'information':
          return { type: 'live_response', prompt: intent.data };
        case 'tool_request':
          return this.handleToolRequest(intent.data.tool, intent.data.args, context);
        case 'unknown':
          return {
            type: 'error',
            message: intent.data || 'I could not understand that request.',
            recoverable: true,
          };
        default:
          return { type: 'error', message: 'I could not understand that request.', recoverable: true };
      }
    } catch (error) {
      return this.errorResult(error);
    }
  }

  private async handleSpotify(query: string): Promise<AgentResult> {
    try {
      const uri = await this.spotifyService.resolveSpotifyUri(query);
      if (!uri) {
        return { type: 'error', message: 'I could not find that on Spotify.', recoverable: true };
      }
      return {
        type: 'device_command',
        command: { type: 'intent', intent: 'spotify', data: uri },
      };
    } catch (error) {
      if (error instanceof SpotifyResolutionError) {
        return { type: 'error', message: error.userMessage, recoverable: true };
      }
      return { type: 'error', message: 'I could not find that on Spotify.', recoverable: true };
    }
  }

  private async handleToolRequest(
    toolName: string,
    args: Record<string, unknown>,
    context: AgentContext,
  ): Promise<AgentResult> {
    if (!this.toolRegistry.has(toolName)) {
      throw new ToolNotFoundError(toolName);
    }

    const toolContext = {
      sessionId: context.sessionId,
      timezone: context.timezone,
      ...(context.userId ? { userId: context.userId } : {}),
      now: new Date(),
    };

    const result = await this.calendarToolService.executeRaw(toolName, args, toolContext);
    const text = this.formatToolResult(toolName, result);
    this.logger.log('AGENT', 'info', `result=spoken_response chars=${text.length}`);
    return { type: 'spoken_response', text };
  }

  private formatToolResult(toolName: string, result: unknown): string {
    switch (toolName) {
      case 'get_daily_agenda':
        return formatDailyAgendaForSpeech(result as GetDailyAgendaResult);
      case 'get_next_calendar_event':
        return formatNextEventForSpeech(result as GetNextCalendarEventResult);
      case 'get_calendar_events':
        return formatCalendarEventsForSpeech(result as GetCalendarEventsResult);
      default:
        return 'Done.';
    }
  }

  private errorResult(error: unknown): AgentResult {
    if (error instanceof AgentError) {
      this.logger.log('AGENT', 'warn', error.message);
      return { type: 'error', message: error.userMessage, recoverable: error.recoverable };
    }
    if (error instanceof CalendarNotConnectedError) {
      return { type: 'error', message: error.userMessage, recoverable: true };
    }
    if (error instanceof CalendarProviderError) {
      return { type: 'error', message: error.userMessage, recoverable: true };
    }
    if (error instanceof ToolNotFoundError) {
      return { type: 'error', message: error.userMessage, recoverable: true };
    }
    this.logger.log('AGENT', 'error', String(error));
    return { type: 'error', message: 'Something went wrong.', recoverable: true };
  }
}
