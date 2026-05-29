import { Injectable } from '@nestjs/common';
import { WebSocket } from 'ws';
import { SpeakerSessionFactory } from './speaker-session.factory';
import { SpeakerSession } from './speaker-session';
import { SpeakerNodeConnection } from './speaker-node-connection';
import { SpeakerRouter } from './speaker-router';
import { AppLoggerService } from '../logging/logger.module';

interface LogicalSpeakerEntry {
  speakerId: string;
  session: SpeakerSession;
  router: SpeakerRouter;
  nodes: Map<WebSocket, SpeakerNodeConnection>;
}

@Injectable()
export class SpeakerSessionRegistry {
  private readonly sessions = new Map<string, LogicalSpeakerEntry>();
  private readonly nodeToSpeaker = new Map<WebSocket, string>();

  constructor(
    private readonly sessionFactory: SpeakerSessionFactory,
    private readonly logger: AppLoggerService,
  ) {}

  async registerNode(node: SpeakerNodeConnection): Promise<SpeakerSession> {
    let entry = this.sessions.get(node.speakerId);
    if (!entry) {
      const router = new SpeakerRouter(node.speakerId, this.logger);
      const session = await this.sessionFactory.create(node.speakerId, router);
      entry = {
        speakerId: node.speakerId,
        session,
        router,
        nodes: new Map(),
      };
      this.sessions.set(node.speakerId, entry);
      this.logger.log('SESSION', 'info', `[SESSION] logical speaker created speaker_id=${node.speakerId}`);
    }

    const replaced = [...entry.nodes.values()].find((existing) => existing.nodeId === node.nodeId);
    if (replaced) {
      entry.nodes.delete(replaced.ws);
      this.nodeToSpeaker.delete(replaced.ws);
      entry.router.detach(replaced);
    }

    entry.nodes.set(node.ws, node);
    this.nodeToSpeaker.set(node.ws, node.speakerId);
    entry.router.attach(node);
    this.logger.log(
      'SESSION',
      'info',
      `[NODE] connected speaker_id=${node.speakerId} node_id=${node.nodeId} role=${node.role} board_profile=${node.boardProfile}`,
    );
    this.logger.log('SESSION', 'info', `[NODE] capabilities=${node.capabilities.join(',')}`);
    this.logger.log(
      'SESSION',
      'info',
      `[SESSION] logical speaker ready speaker_id=${node.speakerId} playback=${entry.router.hasPlaybackNode()} voice_ui=${entry.router.hasVoiceUiNode()}`,
    );
    return entry.session;
  }

  getNode(ws: WebSocket): SpeakerNodeConnection | null {
    const speakerId = this.nodeToSpeaker.get(ws);
    if (!speakerId) {
      return null;
    }
    return this.sessions.get(speakerId)?.nodes.get(ws) ?? null;
  }

  getSessionForNode(ws: WebSocket): SpeakerSession | null {
    const speakerId = this.nodeToSpeaker.get(ws);
    if (!speakerId) {
      return null;
    }
    return this.sessions.get(speakerId)?.session ?? null;
  }

  async unregisterSocket(ws: WebSocket): Promise<void> {
    const speakerId = this.nodeToSpeaker.get(ws);
    if (!speakerId) {
      return;
    }
    const entry = this.sessions.get(speakerId);
    const node = entry?.nodes.get(ws);
    this.nodeToSpeaker.delete(ws);
    if (!entry || !node) {
      return;
    }
    entry.nodes.delete(ws);
    entry.router.detach(node);
    this.logger.log('SESSION', 'info', `[NODE] disconnected speaker_id=${speakerId} node_id=${node.nodeId}`);
    if (entry.nodes.size === 0) {
      this.sessions.delete(speakerId);
      await entry.session.cleanup();
      this.logger.log('SESSION', 'info', `[SESSION] logical speaker removed speaker_id=${speakerId}`);
    }
  }

  async cleanup(): Promise<void> {
    await Promise.allSettled([...this.sessions.values()].map((entry) => entry.session.cleanup()));
    this.sessions.clear();
    this.nodeToSpeaker.clear();
  }
}
