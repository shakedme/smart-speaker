import WebRtcAec3 from '@ennuicastr/webrtcaec3.js';
import {bufferToFloat32Array, UPLINK_SAMPLE_RATE} from './pcm-utils';
import {
    AEC3_FRAME_SAMPLES,
    ParsedUplinkFrame,
    splitAec3Frames,
    UPLINK_FRAME_TYPE_MIC,
    UPLINK_FRAME_TYPE_PLAYBACK_REF,
} from './uplink-frame';

type Aec3Module = Awaited<ReturnType<typeof WebRtcAec3>>;
type Aec3Instance = InstanceType<Aec3Module['AEC3']>;

interface TimestampedFrame extends ParsedUplinkFrame {
    splitIndex: number;
}

const DEFAULT_INITIAL_DELAY_MS = parseInt(process.env.AEC_INITIAL_DELAY_MS ?? '100', 10);
const AEC3_FRAME_DURATION_US = Math.round((AEC3_FRAME_SAMPLES * 1_000_000) / UPLINK_SAMPLE_RATE);

export class BackendAec3Processor {
    private aec: Aec3Instance;
    private readonly initialDelayUs: number;
    private readonly renderBuffer: TimestampedFrame[] = [];
    private readonly captureBuffer: TimestampedFrame[] = [];
    private readonly silenceRender = new Float32Array(AEC3_FRAME_SAMPLES);
    private readonly outFloat = new Float32Array(AEC3_FRAME_SAMPLES);

    private lastRenderTimestampUs = 0;
    private renderFramesFed = 0;
    private captureFramesProcessed = 0;

    private constructor(aec: Aec3Instance, initialDelayMs: number) {
        this.aec = aec;
        this.initialDelayUs = Math.max(0, initialDelayMs) * 1000;
        this.aec.setAudioBufferDelay(Math.max(0, initialDelayMs));
    }

    static async createFromEnv(): Promise<BackendAec3Processor | null> {
        const enabled = process.env.BACKEND_AEC3_ENABLED === '1' ||
            process.env.BACKEND_AEC3_ENABLED === 'true';
        if (!enabled) {
            return null;
        }

        const module = await WebRtcAec3();
        const aec = new module.AEC3(UPLINK_SAMPLE_RATE, 1, 1);
        return new BackendAec3Processor(aec, DEFAULT_INITIAL_DELAY_MS);
    }

    pushFrame(frame: ParsedUplinkFrame): Buffer[] {
        const chunks = splitAec3Frames(frame).map((split, index) => ({
            ...split,
            splitIndex: index,
        }));

        if (frame.frameType === UPLINK_FRAME_TYPE_PLAYBACK_REF) {
            this.renderBuffer.push(...chunks);
            this.renderBuffer.sort((a, b) =>
                a.timestampUs - b.timestampUs || a.sequence - b.sequence || a.splitIndex - b.splitIndex);
            this.trimBuffer(this.renderBuffer, 200);
            return [];
        }

        if (frame.frameType === UPLINK_FRAME_TYPE_MIC) {
            this.captureBuffer.push(...chunks);
            this.captureBuffer.sort((a, b) =>
                a.timestampUs - b.timestampUs || a.sequence - b.sequence || a.splitIndex - b.splitIndex);
            return this.drainCapture();
        }

        return [];
    }

    private trimBuffer(buffer: TimestampedFrame[], maxEntries: number): void {
        if (buffer.length > maxEntries) {
            buffer.splice(0, buffer.length - maxEntries);
        }
    }

    private drainCapture(): Buffer[] {
        const outputs: Buffer[] = [];

        while (this.captureBuffer.length > 0) {
            const capture = this.captureBuffer[0]!;
            if (capture.numSamples !== AEC3_FRAME_SAMPLES) {
                outputs.push(Buffer.from(capture.pcm));
                this.captureBuffer.shift();
                continue;
            }

            this.feedRenderThrough(capture.timestampUs - this.initialDelayUs);
            const captureFloat = bufferToFloat32Array(capture.pcm);
            if (captureFloat.length < AEC3_FRAME_SAMPLES) {
                outputs.push(Buffer.from(capture.pcm));
                this.captureBuffer.shift();
                continue;
            }

            this.aec.process(
                [this.outFloat],
                [captureFloat.subarray(0, AEC3_FRAME_SAMPLES)],
                {sampleRateIn: UPLINK_SAMPLE_RATE},
            );
            outputs.push(this.float32ToPcm(this.outFloat));
            this.captureFramesProcessed++;
            this.captureBuffer.shift();
        }

        return outputs;
    }

    private feedRenderThrough(untilTimestampUs: number): void {
        if (this.lastRenderTimestampUs === 0) {
            const firstRenderTimestamp = this.renderBuffer[0]?.timestampUs;
            this.lastRenderTimestampUs = (firstRenderTimestamp ?? untilTimestampUs) - AEC3_FRAME_DURATION_US;
        }

        while (this.renderBuffer.length > 0 &&
            this.renderBuffer[0]!.timestampUs <= untilTimestampUs) {
            const nextRender = this.renderBuffer[0]!;
            while (this.lastRenderTimestampUs + AEC3_FRAME_DURATION_US < nextRender.timestampUs) {
                this.analyzeSilenceFrame(this.lastRenderTimestampUs + AEC3_FRAME_DURATION_US);
            }
            this.analyzeRenderFrame(this.renderBuffer.shift()!);
        }

        while (this.lastRenderTimestampUs + AEC3_FRAME_DURATION_US <= untilTimestampUs) {
            this.analyzeSilenceFrame(this.lastRenderTimestampUs + AEC3_FRAME_DURATION_US);
        }
    }

    private analyzeSilenceFrame(timestampUs: number): void {
        this.aec.analyze([this.silenceRender], {sampleRateIn: UPLINK_SAMPLE_RATE});
        this.renderFramesFed++;
        this.lastRenderTimestampUs = timestampUs;
    }

    private analyzeRenderFrame(render: ParsedUplinkFrame): void {
        const renderFloat = bufferToFloat32Array(render.pcm);
        if (renderFloat.length >= AEC3_FRAME_SAMPLES) {
            this.aec.analyze(
                [renderFloat.subarray(0, AEC3_FRAME_SAMPLES)],
                {sampleRateIn: UPLINK_SAMPLE_RATE},
            );
        } else {
            this.aec.analyze([this.silenceRender], {sampleRateIn: UPLINK_SAMPLE_RATE});
        }
        this.renderFramesFed++;
        this.lastRenderTimestampUs = render.timestampUs;
    }

    private float32ToPcm(samples: Float32Array): Buffer {
        const out = Buffer.allocUnsafe(samples.length * 2);
        for (let i = 0; i < samples.length; i++) {
            const clamped = Math.max(-1, Math.min(1, samples[i]!));
            out.writeInt16LE(Math.round(clamped * 32767), i * 2);
        }
        return out;
    }

    destroy(): void {
        this.aec.free();
        this.renderBuffer.length = 0;
        this.captureBuffer.length = 0;
    }

    getStats(): {
        renderFramesFed: number;
        captureFramesProcessed: number;
        renderQueued: number;
        captureQueued: number;
    } {
        return {
            renderFramesFed: this.renderFramesFed,
            captureFramesProcessed: this.captureFramesProcessed,
            renderQueued: this.renderBuffer.length,
            captureQueued: this.captureBuffer.length,
        };
    }
}
