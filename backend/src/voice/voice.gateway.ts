import {
  OnGatewayConnection,
  OnGatewayDisconnect,
  WebSocketGateway,
} from '@nestjs/websockets';
import { Injectable, OnModuleDestroy } from '@nestjs/common';
import { WebSocket } from 'ws';
import { IncomingMessage } from 'http';
import { SpeakerSessionFactory } from './speaker-session.factory';
import { SpeakerSession } from './speaker-session';
import { AppLoggerService } from '../logging/logger.module';

@Injectable()
@WebSocketGateway({ path: '/voice' })
export class VoiceGateway implements OnGatewayConnection, OnGatewayDisconnect, OnModuleDestroy {
  private readonly activeSessions = new Map<WebSocket, SpeakerSession>();

  constructor(
    private readonly sessionFactory: SpeakerSessionFactory,
    private readonly logger: AppLoggerService,
  ) {}

  async handleConnection(client: WebSocket, request: IncomingMessage): Promise<void> {
    request.socket.setNoDelay(true);
    const clientIP = request.socket.remoteAddress;
    this.logger.log('SESSION', 'info', `New WebSocket connection from ${clientIP ?? 'unknown'}`);

    try {
      const session = await this.sessionFactory.create(client);
      this.activeSessions.set(client, session);

      client.on('message', (data: Buffer | ArrayBuffer | Buffer[], isBinary: boolean) => {
        try {
          const buffer = Buffer.isBuffer(data)
            ? data
            : Buffer.from(data as ArrayBuffer);
          session.handleMessage(buffer, isBinary);
        } catch (error) {
          this.logger.log('SESSION', 'error', `Error handling message: ${error}`);
          client.close(1011, 'Internal server error');
        }
      });

      client.on('error', (error) => {
        this.logger.log('SESSION', 'error', `WebSocket error: ${error}`);
      });
    } catch (error) {
      this.logger.log('SESSION', 'error', `Failed to create session: ${error}`);
      client.close(1011, 'Failed to initialize voice session');
    }
  }

  async handleDisconnect(client: WebSocket): Promise<void> {
    const session = this.activeSessions.get(client);
    if (session) {
      this.activeSessions.delete(client);
      await session.cleanup();
    }
    this.logger.log('SESSION', 'info', 'WebSocket connection closed');
  }

  async onModuleDestroy(): Promise<void> {
    await Promise.allSettled(
      [...this.activeSessions.values()].map((session) => session.cleanup()),
    );
    this.activeSessions.clear();
  }
}
