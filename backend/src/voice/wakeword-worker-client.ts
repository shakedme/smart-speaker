import {spawn, ChildProcessWithoutNullStreams} from 'node:child_process';
import {createInterface} from 'node:readline';
import * as fs from 'node:fs';
import path from 'node:path';
import {performance} from 'node:perf_hooks';

export interface WakewordPredictResult {
    score: number;
    model: string;
    /** ms from predict() entry until result (queue wait + Python/ONNX). */
    predictMs: number;
}

interface WorkerResponse {
    ok: boolean;
    score?: number;
    model?: string;
    error?: string;
    ready?: boolean;
}

type QueuedWork =
    | {
          kind: 'predict';
          request: string;
          predictT0: number;
          resolve: (value: WakewordPredictResult) => void;
          reject: (reason: Error) => void;
      }
    | {
          kind: 'ping';
          request: string;
          resolve: () => void;
          reject: (reason: Error) => void;
      };

/**
 * Shared long-lived Python worker that runs LiveKit WakeWordModel inference.
 */
export class WakewordWorkerClient {
    private process: ChildProcessWithoutNullStreams | null = null;
    private readonly modelPath: string;
    private readonly pythonBin: string;
    private readonly workerScript: string;
    private readyPromise: Promise<void> | null = null;
    private pending: QueuedWork | null = null;
    private queue: QueuedWork[] = [];

    constructor(modelPath: string) {
        this.modelPath = WakewordWorkerClient.resolvePath(modelPath);
        this.pythonBin = WakewordWorkerClient.resolvePythonBin();
        this.workerScript = path.join(__dirname, 'wakeword-worker.py');
    }

    private static resolvePath(configuredPath: string): string {
        return path.isAbsolute(configuredPath)
            ? configuredPath
            : path.resolve(process.cwd(), configuredPath);
    }

    private static resolvePythonBin(): string {
        const configured = process.env.WAKEWORD_PYTHON;
        if (configured) {
            return WakewordWorkerClient.resolvePath(configured);
        }
        const venvPython = path.resolve(process.cwd(), '.venv/bin/python');
        if (fs.existsSync(venvPython)) {
            return venvPython;
        }
        return 'python3.11';
    }

    static resolveDefaultModelPath(): string {
        if (process.env.WAKEWORD_MODEL_PATH) {
            return WakewordWorkerClient.resolvePath(process.env.WAKEWORD_MODEL_PATH);
        }
        return path.resolve(process.cwd(), 'models/wakeword/output/okay_rufus/okay_rufus.onnx');
    }

    async start(): Promise<void> {
        if (this.readyPromise) {
            return this.readyPromise;
        }

        this.readyPromise = new Promise<void>((resolve, reject) => {
            const child = spawn(
                this.pythonBin,
                [this.workerScript, '--model', this.modelPath],
                {stdio: ['pipe', 'pipe', 'pipe']},
            );
            this.process = child;

            child.stderr.on('data', (chunk: Buffer) => {
                console.error(`[WAKEWORD_WORKER] ${chunk.toString().trim()}`);
            });

            child.on('error', (err) => {
                reject(new Error(`Failed to spawn wakeword worker: ${err.message}`));
            });

            child.on('exit', (code, signal) => {
                const reason = signal ? `signal ${signal}` : `code ${code}`;
                this.rejectAllPending(new Error(`Wakeword worker exited (${reason})`));
                this.process = null;
                this.readyPromise = null;
            });

            const rl = createInterface({input: child.stdout});
            rl.on('line', (line) => this.handleWorkerLine(line, resolve, reject));
        });

        return this.readyPromise;
    }

    private handleWorkerLine(line: string, onReady: () => void, onReadyError: (err: Error) => void): void {
        let parsed: WorkerResponse;
        try {
            parsed = JSON.parse(line) as WorkerResponse;
        } catch {
            console.error(`[WAKEWORD_WORKER] Invalid JSON: ${line}`);
            return;
        }

        if (parsed.ready) {
            if (parsed.ok) {
                onReady();
            } else {
                onReadyError(new Error(parsed.error ?? 'Worker failed to initialize'));
            }
            return;
        }

        const pending = this.pending;
        this.pending = null;
        if (!pending) {
            console.warn('[WAKEWORD_WORKER] Unexpected response with no pending request');
            this.pumpQueue();
            return;
        }

        if (!parsed.ok) {
            pending.reject(new Error(parsed.error ?? 'Wake word inference failed'));
            this.pumpQueue();
            return;
        }

        if (pending.kind === 'ping') {
            pending.resolve();
            this.pumpQueue();
            return;
        }

        const predictMs = performance.now() - pending.predictT0;
        if (parsed.score !== undefined) {
            pending.resolve({
                score: parsed.score,
                model: parsed.model ?? 'okay_rufus',
                predictMs,
            });
        } else {
            pending.resolve({score: 0, model: parsed.model ?? 'okay_rufus', predictMs});
        }
        this.pumpQueue();
    }

    private pumpQueue(): void {
        if (this.pending || this.queue.length === 0 || !this.process?.stdin.writable) {
            return;
        }
        const next = this.queue.shift()!;
        this.pending = next;
        this.process.stdin.write(next.request);
    }

    private rejectAllPending(err: Error): void {
        if (this.pending) {
            this.pending.reject(err);
            this.pending = null;
        }
        for (const item of this.queue) {
            item.reject(err);
        }
        this.queue = [];
    }

    async predict(pcm: Buffer): Promise<WakewordPredictResult> {
        await this.start();
        if (!this.process) {
            throw new Error('Wakeword worker is not running');
        }

        const request = `${JSON.stringify({
            cmd: 'predict',
            pcm_b64: pcm.toString('base64'),
        })}\n`;

        const predictT0 = performance.now();
        return new Promise<WakewordPredictResult>((resolve, reject) => {
            this.queue.push({
                kind: 'predict',
                request,
                predictT0,
                resolve,
                reject,
            });
            this.pumpQueue();
        });
    }

    async ping(): Promise<void> {
        await this.start();
        if (!this.process) {
            throw new Error('Wakeword worker is not running');
        }

        const request = `${JSON.stringify({cmd: 'ping'})}\n`;
        await new Promise<void>((resolve, reject) => {
            this.queue.push({
                kind: 'ping',
                request,
                resolve,
                reject,
            });
            this.pumpQueue();
        });
    }

    shutdown(): void {
        if (this.process) {
            this.process.kill('SIGTERM');
            this.process = null;
        }
        this.readyPromise = null;
        this.rejectAllPending(new Error('Wakeword worker shut down'));
    }
}

let sharedWorker: WakewordWorkerClient | null = null;

export function getSharedWakewordWorker(): WakewordWorkerClient {
    if (!sharedWorker) {
        sharedWorker = new WakewordWorkerClient(WakewordWorkerClient.resolveDefaultModelPath());
    }
    return sharedWorker;
}

export async function shutdownSharedWakewordWorker(): Promise<void> {
    sharedWorker?.shutdown();
    sharedWorker = null;
}
