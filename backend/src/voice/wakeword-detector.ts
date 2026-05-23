import {
    concatPcmBuffers,
    fitPcmWindow,
    pcmBytesForDurationMs,
} from '../audio/pcm-utils';
import {loadWakewordThreshold} from './wakeword-config';
import {WakewordWorkerClient} from './wakeword-worker-client';

export interface WakewordDetection {
    score: number;
    model: string;
    preRollPcm: Buffer;
}

export interface WakewordInferenceResult {
    score: number;
    model: string;
    detection: WakewordDetection | null;
    /** ms for worker round-trip (queue + Python/ONNX); see WakewordPredictResult.predictMs */
    predictMs: number;
}

export interface WakewordDetectorOptions {
    windowMs?: number;
    threshold?: number;
    debounceMs?: number;
    inferenceIntervalMs?: number;
    sampleRate?: number;
}

/**
 * Rolling-window wake word detector backed by the LiveKit ONNX worker.
 */
export class WakewordDetector {
    private readonly worker: WakewordWorkerClient;
    private readonly windowMs: number;
    private readonly windowBytes: number;
    private readonly threshold: number;
    private readonly debounceMs: number;
    private readonly inferenceIntervalMs: number;

    private ringChunks: Buffer[] = [];
    private inferenceInFlight = false;
    private pendingWindow: Buffer | null = null;
    private lastDetectionMs = 0;
    private lastInferenceMs = 0;

    constructor(worker: WakewordWorkerClient, options?: WakewordDetectorOptions) {
        this.worker = worker;
        this.windowMs = options?.windowMs
            ?? parseInt(process.env.WAKEWORD_WINDOW_MS ?? '2000', 10);
        const sampleRate = options?.sampleRate ?? 16000;
        this.windowBytes = pcmBytesForDurationMs(this.windowMs, sampleRate);
        this.threshold = options?.threshold ?? loadWakewordThreshold();
        this.debounceMs = options?.debounceMs
            ?? parseInt(process.env.WAKEWORD_DEBOUNCE_MS ?? '2000', 10);
        this.inferenceIntervalMs = options?.inferenceIntervalMs
            // Default ~4× typical Python predict_ms (~20ms); raise if worker falls behind.
            ?? parseInt(process.env.WAKEWORD_INFERENCE_INTERVAL_MS ?? '80', 10);
    }

    get detectionThreshold(): number {
        return this.threshold;
    }

    getRingStats(): {bytes: number; targetBytes: number} {
        return {
            bytes: this.getWindow().length,
            targetBytes: this.windowBytes,
        };
    }

    /**
     * Why `hasPendingInference()` is false (for diagnostics). Returns `ready` if inference can run now.
     */
    getInferenceBlockReason(): string {
        if (this.inferenceInFlight) {
            return 'inference_in_flight';
        }
        if (!this.pendingWindow) {
            const w = this.getWindow().length;
            return `no_pending_window ring_bytes=${w}/${this.windowBytes}`;
        }
        const elapsed = Date.now() - this.lastInferenceMs;
        if (elapsed < this.inferenceIntervalMs) {
            return `rate_limited wait_ms=${Math.ceil(this.inferenceIntervalMs - elapsed)}`;
        }
        return 'ready';
    }

    reset(): void {
        this.ringChunks = [];
        this.pendingWindow = null;
        this.lastInferenceMs = 0;
    }

    push(pcm: Buffer): void {
        if (pcm.length === 0) {
            return;
        }

        const combined = this.ringChunks.length === 0
            ? pcm
            : Buffer.concat([this.getWindow(), pcm]);

        if (combined.length > this.windowBytes) {
            this.ringChunks = [combined.subarray(combined.length - this.windowBytes)];
        } else {
            this.ringChunks = [combined];
        }

        this.refreshPendingWindow();
    }

    getPreRollPcm(): Buffer {
        return concatPcmBuffers(this.ringChunks);
    }

    private getWindow(): Buffer {
        return concatPcmBuffers(this.ringChunks);
    }

    private refreshPendingWindow(): void {
        const window = this.getWindow();
        // fitPcmWindow left-pads shorter buffers; tolerate one chunk of slack.
        const minReadyBytes = this.windowBytes - 512;
        if (window.length >= minReadyBytes) {
            this.pendingWindow = fitPcmWindow(window, this.windowBytes);
        }
    }

    hasPendingInference(): boolean {
        if (!this.pendingWindow || this.inferenceInFlight) {
            return false;
        }
        return Date.now() - this.lastInferenceMs >= this.inferenceIntervalMs;
    }

    async runPendingInference(): Promise<WakewordInferenceResult | null> {
        if (!this.hasPendingInference()) {
            return null;
        }

        this.inferenceInFlight = true;
        this.lastInferenceMs = Date.now();
        const window = this.pendingWindow!;
        this.pendingWindow = null;

        try {
            const result = await this.worker.predict(window);
            const now = Date.now();
            let detection: WakewordDetection | null = null;

            if (result.score >= this.threshold && now - this.lastDetectionMs >= this.debounceMs) {
                this.lastDetectionMs = now;
                detection = {
                    score: result.score,
                    model: result.model,
                    preRollPcm: this.getPreRollPcm(),
                };
            }

            return {
                score: result.score,
                model: result.model,
                detection,
                predictMs: result.predictMs,
            };
        } finally {
            this.inferenceInFlight = false;
            this.refreshPendingWindow();
        }
    }
}
