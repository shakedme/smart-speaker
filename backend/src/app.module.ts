import { Module } from '@nestjs/common';
import { AppConfigModule } from './config/config.module';
import { LoggingModule } from './logging/logger.module';
import { VoiceModule } from './voice/voice.module';
import { HealthController } from './health/health.controller';

@Module({
  imports: [AppConfigModule, LoggingModule, VoiceModule],
  controllers: [HealthController],
})
export class AppModule {}
