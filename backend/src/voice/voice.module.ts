import { Module } from '@nestjs/common';
import { VoiceGateway } from './voice.gateway';
import { SpeakerSessionFactory } from './speaker-session.factory';
import { WakewordLifecycleService } from './wakeword-lifecycle.service';
import { AgentModule } from '../agent/agent.module';
import { IntentModule } from '../intent/intent.module';

@Module({
  imports: [AgentModule, IntentModule],
  providers: [WakewordLifecycleService, SpeakerSessionFactory, VoiceGateway],
  exports: [WakewordLifecycleService],
})
export class VoiceModule {}
