import { Injectable } from '@nestjs/common';
import { GoogleGenAI, Part } from '@google/genai';
import { AppConfigService } from '../config/app-config.service';
import { AppLoggerService } from '../logging/logger.module';
import { WAKE_PHRASE_HINT } from '../audio/audio-buffer-utils';
import { ClassifiedIntent, parseClassifiedIntent } from './intent-types';
import { IntentClassifier } from './intent-classifier.interface';
import { GEMINI_CLIENT } from '../agent/agent.tokens';
import { Inject } from '@nestjs/common';

@Injectable()
export class GeminiIntentClassifierService implements IntentClassifier {
  constructor(
    @Inject(GEMINI_CLIENT) private readonly gemini: GoogleGenAI,
    private readonly config: AppConfigService,
    private readonly logger: AppLoggerService,
  ) {}

  async classify(wavBuffer: Buffer, timezone: string): Promise<ClassifiedIntent> {
    const systemInstruction = this.buildSystemInstruction(timezone);
    const contents: Part[] = [
      { text: systemInstruction },
      {
        inlineData: {
          mimeType: 'audio/wav',
          data: wavBuffer.toString('base64'),
        },
      },
    ];

    this.logger.log('INTENT', 'info', `Sending audio to Gemini for intent detection (timezone=${timezone})`);

    try {
      const response = await this.gemini.models.generateContent({
        model: 'gemini-3.1-flash-lite',
        contents,
      });

      const text = response.text;
      if (!text) {
        this.logger.log('INTENT', 'error', 'Gemini response is empty');
        return { intent: 'unknown', data: 'empty response' };
      }

      this.logger.log('INTENT', 'info', `Gemini raw response: ${text}`);
      const raw = JSON.parse(text) as unknown;
      const classified = parseClassifiedIntent(raw);

      if (classified.intent === 'tool_request') {
        this.logger.log(
          'INTENT',
          'info',
          `classified intent=tool_request tool=${classified.data.tool}`,
        );
      } else {
        this.logger.log('INTENT', 'info', `classified intent=${classified.intent}`);
      }

      return classified;
    } catch (error) {
      this.logger.log('INTENT', 'error', `Classification failed: ${error}`);
      return { intent: 'unknown', data: String(error) };
    }
  }

  private buildSystemInstruction(timezone: string): string {
    return `You are a smart home assistant intent detection system.
Your task is to analyze the user's speech and determine their intent.
Respond with a single, minified JSON object with the format: {"intent": "...", "data": "..."}.
Do not include any other text, markdown, or explanation.

Possible intents are "spotify", "playback_control", "information", and "tool_request".
- For "spotify", the "data" should be the song, artist, album or playlist name. For example, "the wall by pink floyd", "jazz", "blackbird", etc. The word PLAY must be included in the intent. DO NOT return the word play in the data property.
- For "playback_control", the "data" should be the action ("pause", "next", "volume up/down").
- For "information", the "data" should be the user's full question, and clearly not be a command.
- For "tool_request" only, "data" is a JSON object (not a string) with "tool" and "args". Always include "timezone": "${timezone}" in args.

Use "tool_request" (not "information") for schedule, agenda, calendar, or meeting questions.

Calendar tool_request examples:
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"today","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"tomorrow","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"monday","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"next monday","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"this week","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_daily_agenda","args":{"date":"next week","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_next_calendar_event","args":{"timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_calendar_events","args":{"startTime":"after_lunch","endTime":"end_of_day","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_calendar_events","args":{"startTime":"afternoon_start","endTime":"afternoon_end","timezone":"${timezone}"}}}
{"intent":"tool_request","data":{"tool":"get_calendar_events","args":{"startTime":"15:00","endTime":"16:00","timezone":"${timezone}"}}}

Ignore any leading wake phrase such as "${WAKE_PHRASE_HINT}" when determining intent.`;
  }
}
