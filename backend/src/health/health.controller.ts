import { Controller, Get } from '@nestjs/common';
import { WakewordLifecycleService } from '../voice/wakeword-lifecycle.service';

@Controller()
export class HealthController {
  constructor(private readonly wakewordLifecycle: WakewordLifecycleService) {}

  @Get('health')
  health(): { status: string; wakeword: boolean } {
    return {
      status: 'ok',
      wakeword: this.wakewordLifecycle.getWorker() !== null,
    };
  }
}
