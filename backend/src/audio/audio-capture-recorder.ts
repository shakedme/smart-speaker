import * as fs from 'node:fs';
import path from 'node:path';
import {UPLINK_SAMPLE_RATE} from './pcm-utils';

interface AudioCaptureMetadata {
    sessionId: string;
    startedAt: string;
    sampleRate: number;
    format: 's16le';
    channels: 1;
    pcmPath: string;
    chunks: number;
    bytes: number;
    lastSyncedAt?: string;
}

export class AudioCaptureRecorder {
    private readonly fd: number;
    private readonly metadataPath: string;
    private readonly metadata: AudioCaptureMetadata;
    private readonly fsyncIntervalMs: number;
    private closed = false;
    private bytesSinceFsync = 0;
    private lastFsyncMs = 0;

    private constructor(dir: string, sessionId: string) {
        fs.mkdirSync(dir, {recursive: true});

        const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
        const baseName = `${timestamp}_${sessionId}`;
        const pcmPath = path.join(dir, `${baseName}.pcm`);
        this.metadataPath = path.join(dir, `${baseName}.json`);
        this.fd = fs.openSync(pcmPath, 'wx');
        this.fsyncIntervalMs = parseInt(process.env.WAKEWORD_CAPTURE_FSYNC_MS ?? '1000', 10);
        this.metadata = {
            sessionId,
            startedAt: new Date().toISOString(),
            sampleRate: UPLINK_SAMPLE_RATE,
            format: 's16le',
            channels: 1,
            pcmPath,
            chunks: 0,
            bytes: 0,
        };
        this.writeMetadata();
    }

    static createFromEnv(sessionId: string): AudioCaptureRecorder | null {
        if (process.env.WAKEWORD_CAPTURE_AUDIO !== '1') {
            return null;
        }

        const captureDir = path.resolve(
            process.cwd(),
            process.env.WAKEWORD_CAPTURE_DIR ?? 'models/wakeword/captures/raw',
        );
        return new AudioCaptureRecorder(captureDir, sessionId);
    }

    get outputPath(): string {
        return this.metadata.pcmPath;
    }

    append(pcm: Buffer): void {
        if (this.closed || pcm.length === 0) {
            return;
        }

        this.metadata.chunks++;
        this.metadata.bytes += pcm.length;
        this.bytesSinceFsync += pcm.length;
        fs.writeSync(this.fd, pcm);
        this.maybeSyncToDisk();
    }

    close(): void {
        if (this.closed) {
            return;
        }

        this.closed = true;
        this.syncToDisk(true);
        fs.closeSync(this.fd);
    }

    private maybeSyncToDisk(): void {
        const now = Date.now();
        if (this.bytesSinceFsync === 0) {
            return;
        }
        if (now - this.lastFsyncMs < this.fsyncIntervalMs) {
            return;
        }
        this.syncToDisk(false);
    }

    private syncToDisk(final: boolean): void {
        try {
            fs.fsyncSync(this.fd);
            this.lastFsyncMs = Date.now();
            this.bytesSinceFsync = 0;
            this.metadata.lastSyncedAt = new Date().toISOString();
            this.writeMetadata();
        } catch (error) {
            if (!final) {
                console.warn(`[WAKEWORD_CAPTURE] fsync failed: ${error}`);
            }
        }
    }

    private writeMetadata(): void {
        fs.writeFileSync(this.metadataPath, JSON.stringify(this.metadata, null, 2));
    }
}
