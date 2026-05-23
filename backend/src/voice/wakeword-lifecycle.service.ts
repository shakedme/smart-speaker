import { Injectable, OnModuleDestroy, OnModuleInit } from '@nestjs/common';
import {
  getSharedWakewordWorker,
  shutdownSharedWakewordWorker,
  WakewordWorkerClient,
} from './wakeword-worker-client';
import { loadWakewordEvalMetrics, loadWakewordThreshold } from './wakeword-config';
import { AppLoggerService } from '../logging/logger.module';

@Injectable()
export class WakewordLifecycleService implements OnModuleInit, OnModuleDestroy {
  private readonly worker: WakewordWorkerClient = getSharedWakewordWorker();
  private started = false;

  constructor(private readonly logger: AppLoggerService) {}

  async onModuleInit(): Promise<void> {
    const threshold = loadWakewordThreshold();
    const evalMetrics = loadWakewordEvalMetrics();
    this.logger.log(
      'AUDIO',
      'info',
      `Wake word threshold: ${threshold}${process.env.WAKEWORD_THRESHOLD ? ' (from env)' : ' (from eval/default)'}`,
    );
    if (evalMetrics) {
      this.logger.log(
        'AUDIO',
        'info',
        `Wake word eval: recall=${((evalMetrics.recall ?? 0) * 100).toFixed(1)}% fpph=${evalMetrics.fpph ?? 'n/a'}`,
      );
    } else {
      this.logger.log('AUDIO', 'warn', 'No okay_rufus_eval.json found.');
    }

    await this.worker.start();
    this.started = true;
    this.logger.log('AUDIO', 'info', 'Wake word worker ready');
  }

  onModuleDestroy(): void {
    if (this.started) {
      shutdownSharedWakewordWorker();
    }
  }

  getWorker(): WakewordWorkerClient {
    return this.worker;
  }
}
