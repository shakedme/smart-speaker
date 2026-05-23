import { WebSocket } from 'ws';
import { v4 as uuidv4 } from 'uuid';
import { GoogleGenAI } from '@google/genai';
import { Injectable } from '@nestjs/common';
import { Inject } from '@nestjs/common';
import { SpeakerSession } from './speaker-session';
import { DeviceMessageSender } from './device-message-sender';
import { WakewordLifecycleService } from './wakeword-lifecycle.service';
import { AppConfigService } from '../config/app-config.service';
import { AppLoggerService } from '../logging/logger.module';
import { AgentOrchestratorService } from '../agent/agent-orchestrator.service';
import { GeminiIntentClassifierService } from '../intent/gemini-intent-classifier.service';
import { GEMINI_CLIENT } from '../agent/agent.tokens';
import { ResponseService } from '../agent/response-service';
import { VadDetector } from './vad-detector';
import { WakewordDetector } from './wakeword-detector';
import { BackendAec3Processor } from '../audio/backend-aec3-processor';

@Injectable()
export class SpeakerSessionFactory {
  constructor(
    private readonly wakewordLifecycle: WakewordLifecycleService,
    private readonly config: AppConfigService,
    private readonly logger: AppLoggerService,
    private readonly agentOrchestrator: AgentOrchestratorService,
    private readonly intentClassifier: GeminiIntentClassifierService,
    @Inject(GEMINI_CLIENT) private readonly gemini: GoogleGenAI,
  ) {}

  async create(ws: WebSocket): Promise<SpeakerSession> {
    const sessionId = uuidv4();
    const deviceSender = new DeviceMessageSender(ws);
    const vadDetector = await VadDetector.create({
      positiveThreshold: this.config.vadThreshold,
    });
    const wakewordDetector = new WakewordDetector(this.wakewordLifecycle.getWorker());
    const aecProcessor = this.config.backendAec3Enabled
      ? await BackendAec3Processor.createFromEnv()
      : null;

    const session = new SpeakerSession({
      ws,
      sessionId,
      deviceSender,
      vadDetector,
      wakewordDetector,
      aecProcessor,
      config: this.config,
      logger: this.logger,
      agentOrchestrator: this.agentOrchestrator,
      intentClassifier: this.intentClassifier,
      gemini: this.gemini,
      createResponseService: (callbacks) =>
        new ResponseService(this.gemini, deviceSender, this.logger, sessionId, callbacks),
    });

    await session.initialize();
    return session;
  }
}
