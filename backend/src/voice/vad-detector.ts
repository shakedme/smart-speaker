import {RealTimeVAD} from 'avr-vad';
import {bufferToFloat32Array, UPLINK_SAMPLE_RATE} from '../audio/pcm-utils';

export interface VadProcessResult {
    voiceProbability: number;
    speechActive: boolean;
}

interface VadFrameState {
    chunkMaxProbability: number;
}

/**
 * Silero VAD wrapper replacing Picovoice Cobra.
 * Processes 16 kHz mono s16le PCM via avr-vad RealTimeVAD.
 */
export class VadDetector {
    private vad: RealTimeVAD;
    private readonly positiveThreshold: number;
    private readonly frameState: VadFrameState;

    private constructor(vad: RealTimeVAD, positiveThreshold: number, frameState: VadFrameState) {
        this.vad = vad;
        this.positiveThreshold = positiveThreshold;
        this.frameState = frameState;
    }

    static async create(options?: {
        positiveThreshold?: number;
        frameSamples?: number;
    }): Promise<VadDetector> {
        const positiveThreshold = options?.positiveThreshold
            ?? parseFloat(process.env.VAD_THRESHOLD ?? '0.5');
        const frameSamples = options?.frameSamples ?? 512;
        const frameState: VadFrameState = {chunkMaxProbability: 0};

        const vad = await RealTimeVAD.new({
            sampleRate: UPLINK_SAMPLE_RATE,
            model: 'v5',
            frameSamples,
            positiveSpeechThreshold: positiveThreshold,
            negativeSpeechThreshold: Math.max(0.1, positiveThreshold - 0.15),
            onFrameProcessed: (probs) => {
                frameState.chunkMaxProbability = Math.max(frameState.chunkMaxProbability, probs.isSpeech);
            },
            onVADMisfire: () => {},
            onSpeechStart: () => {},
            onSpeechRealStart: () => {},
            onSpeechEnd: () => {},
        });
        vad.start();

        return new VadDetector(vad, positiveThreshold, frameState);
    }

    get frameLength(): number {
        return 512;
    }

    /** Threshold above which `speechActive` becomes true (same as VAD_THRESHOLD env). */
    get speechPositiveThreshold(): number {
        return this.positiveThreshold;
    }

    async processPcm(pcm: Buffer): Promise<VadProcessResult> {
        this.frameState.chunkMaxProbability = 0;
        const floatAudio = bufferToFloat32Array(pcm);
        if (floatAudio.length > 0) {
            await this.vad.processAudio(floatAudio);
        }

        const voiceProbability = this.frameState.chunkMaxProbability;
        return {
            voiceProbability,
            speechActive: voiceProbability >= this.positiveThreshold,
        };
    }

    reset(): void {
        this.frameState.chunkMaxProbability = 0;
        this.vad.reset();
    }

    destroy(): void {
        void this.vad.destroy();
    }
}

export {UPLINK_SAMPLE_RATE};
