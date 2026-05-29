import {
  OnGatewayConnection,
  OnGatewayDisconnect,
  WebSocketGateway,
} from '@nestjs/websockets';
import { Injectable, OnModuleDestroy } from '@nestjs/common';
import { WebSocket } from 'ws';
import { IncomingMessage } from 'http';
import { AppLoggerService } from '../logging/logger.module';
import { SpeakerSessionRegistry } from './speaker-session-registry';
import { parseControlMessage, validateHello } from './speaker-protocol';
import { SpeakerNodeConnection } from './speaker-node-connection';

@Injectable()
@WebSocketGateway({ path: '/voice' })
export class VoiceGateway implements OnGatewayConnection, OnGatewayDisconnect, OnModuleDestroy {
  private readonly pendingSockets = new Set<WebSocket>();

  constructor(
    private readonly registry: SpeakerSessionRegistry,
    private readonly logger: AppLoggerService,
  ) {}

  async handleConnection(client: WebSocket, request: IncomingMessage): Promise<void> {
    request.socket.setNoDelay(true);
    const clientIP = request.socket.remoteAddress;
    this.logger.log('SESSION', 'info', `New WebSocket connection from ${clientIP ?? 'unknown'}`);
    this.pendingSockets.add(client);

    try {
      client.on('message', (data: Buffer | ArrayBuffer | Buffer[], isBinary: boolean) => {
        try {
          const buffer = Buffer.isBuffer(data)
            ? data
            : Buffer.from(data as ArrayBuffer);
          void this.handleSocketMessage(client, buffer, isBinary);
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

  private async handleSocketMessage(client: WebSocket, buffer: Buffer, isBinary: boolean): Promise<void> {
    const node = this.registry.getNode(client);
    if (!node) {
      if (isBinary) {
        this.logger.log('SESSION', 'warn', 'Received binary message before hello');
        client.close(1008, 'hello required');
        return;
      }
      const message = parseControlMessage(buffer);
      if (!message) {
        client.close(1008, 'invalid hello');
        return;
      }
      const hello = validateHello(message);
      if (!hello.ok) {
        this.logger.log('SESSION', 'warn', `[NODE] rejected hello: ${hello.error}`);
        client.close(1008, hello.error);
        return;
      }
      const connection = new SpeakerNodeConnection(client, hello.hello);
      await this.registry.registerNode(connection);
      this.pendingSockets.delete(client);
      return;
    }

    const session = this.registry.getSessionForNode(client);
    if (!session) {
      client.close(1011, 'session unavailable');
      return;
    }
    session.handleNodeMessage(node, buffer, isBinary);
  }

  async handleDisconnect(client: WebSocket): Promise<void> {
    this.pendingSockets.delete(client);
    await this.registry.unregisterSocket(client);
    this.logger.log('SESSION', 'info', 'WebSocket connection closed');
  }

  async onModuleDestroy(): Promise<void> {
    await this.registry.cleanup();
    this.pendingSockets.clear();
  }
}
