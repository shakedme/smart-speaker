import { Module } from '@nestjs/common';
import { AgentOrchestratorService } from './agent-orchestrator.service';
import { SpotifyModule } from '../tools/spotify/spotify.module';
import { CalendarModule } from '../tools/calendar/calendar.module';
import { IntentModule } from '../intent/intent.module';
import { ToolsModule } from '../tools/tools.module';

@Module({
  imports: [ToolsModule, SpotifyModule, CalendarModule, IntentModule],
  providers: [AgentOrchestratorService],
  exports: [AgentOrchestratorService, ToolsModule],
})
export class AgentModule {}
