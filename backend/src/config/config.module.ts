import { Global, Module } from '@nestjs/common';
import { ConfigModule as NestConfigModule } from '@nestjs/config';
import path from 'node:path';
import configuration from './configuration';
import { AppConfigService } from './app-config.service';

/** When running from backend/, Nest cwd is the backend root. */
const ENV_FILE_PATHS = [
  path.join(process.cwd(), '.env'),
  path.join(process.cwd(), 'src/.env'),
];

@Global()
@Module({
  imports: [
    NestConfigModule.forRoot({
      isGlobal: true,
      envFilePath: ENV_FILE_PATHS,
      load: [configuration],
      // Validation happens in loadAppConfig() — Nest's validate() receives flat
      // env keys (GOOGLE_AI_API_KEY), not our camelCase AppConfig shape.
    }),
  ],
  providers: [AppConfigService],
  exports: [AppConfigService],
})
export class AppConfigModule {}
