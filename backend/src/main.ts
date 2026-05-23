import 'reflect-metadata';
import { config as loadDotenv } from 'dotenv';
import { resolve } from 'path';
import { NestFactory } from '@nestjs/core';
import { WsAdapter } from '@nestjs/platform-ws';
import { NestExpressApplication } from '@nestjs/platform-express';
import { join } from 'path';
import { AppModule } from './app.module';
import { AppConfigService } from './config/app-config.service';

// Load backend/.env before Nest ConfigModule; override empty shell placeholders.
loadDotenv({ path: resolve(process.cwd(), '.env'), override: true });

async function bootstrap(): Promise<void> {
  const app = await NestFactory.create<NestExpressApplication>(AppModule);
  const config = app.get(AppConfigService);

  app.useWebSocketAdapter(new WsAdapter(app));
  app.enableShutdownHooks();
  app.useStaticAssets(join(__dirname, '../public'));

  await app.listen(config.port, '0.0.0.0');

  console.log('BT Speaker Live Voice Server (NestJS)');
  console.log(`Environment: ${config.nodeEnv}`);
  console.log(`Server running on http://localhost:${config.port}`);
  console.log(`WebSocket endpoint: ws://localhost:${config.port}/voice`);
  console.log(`Frontend: http://localhost:${config.port}`);
}

void bootstrap();
