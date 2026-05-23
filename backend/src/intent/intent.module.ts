import { Module } from '@nestjs/common';
import { GoogleGenAI } from '@google/genai';
import { AppConfigService } from '../config/app-config.service';
import { GEMINI_CLIENT } from '../agent/agent.tokens';
import { GeminiIntentClassifierService } from './gemini-intent-classifier.service';
import { INTENT_CLASSIFIER } from './intent-classifier.interface';

@Module({
  providers: [
    {
      provide: GEMINI_CLIENT,
      useFactory: (config: AppConfigService) =>
        new GoogleGenAI({ apiKey: config.googleAiApiKey }),
      inject: [AppConfigService],
    },
    GeminiIntentClassifierService,
    {
      provide: INTENT_CLASSIFIER,
      useExisting: GeminiIntentClassifierService,
    },
  ],
  exports: [GeminiIntentClassifierService, INTENT_CLASSIFIER, GEMINI_CLIENT],
})
export class IntentModule {}
