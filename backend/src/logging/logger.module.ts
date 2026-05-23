import { Global, Injectable, Module } from '@nestjs/common';
import { LogPrefix } from './log-prefix';

export type LogLevel = 'info' | 'warn' | 'error' | 'debug';

@Injectable()
export class AppLoggerService {
  log(prefix: LogPrefix, level: LogLevel, message: string, sessionId?: string): void {
    const sessionPart = sessionId ? ` session=${sessionId}` : '';
    console.log(`[${prefix}] [${level.toUpperCase()}]${sessionPart} ${message}`);
  }

  forPrefix(prefix: LogPrefix, sessionId?: string) {
    return {
      info: (message: string) => this.log(prefix, 'info', message, sessionId),
      warn: (message: string) => this.log(prefix, 'warn', message, sessionId),
      error: (message: string) => this.log(prefix, 'error', message, sessionId),
      debug: (message: string) => this.log(prefix, 'debug', message, sessionId),
    };
  }
}

@Global()
@Module({
  providers: [AppLoggerService],
  exports: [AppLoggerService],
})
export class LoggingModule {}
