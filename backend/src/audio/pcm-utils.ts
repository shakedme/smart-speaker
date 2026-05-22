/**
 * PCM conversion utilities for 16 kHz mono s16le uplink audio.
 */

export const UPLINK_SAMPLE_RATE = 16000;

/** Convert little-endian s16le PCM buffer to Int16Array (views buffer when aligned). */
export function bufferToInt16Array(pcm: Buffer): Int16Array {
    if (pcm.length === 0) {
        return new Int16Array(0);
    }
    if (pcm.byteOffset % 2 === 0 && pcm.length % 2 === 0) {
        return new Int16Array(pcm.buffer, pcm.byteOffset, pcm.length / 2);
    }
    const out = new Int16Array(pcm.length / 2);
    for (let i = 0; i < out.length; i++) {
        out[i] = pcm.readInt16LE(i * 2);
    }
    return out;
}

/** Convert little-endian s16le PCM to normalized float32 in [-1, 1]. */
export function bufferToFloat32Array(pcm: Buffer): Float32Array {
    const samples = bufferToInt16Array(pcm);
    const out = new Float32Array(samples.length);
    for (let i = 0; i < samples.length; i++) {
        out[i] = samples[i]! / 32768;
    }
    return out;
}

/** Concatenate PCM buffers without copying when only one chunk is present. */
export function concatPcmBuffers(chunks: Buffer[]): Buffer {
    if (chunks.length === 0) {
        return Buffer.alloc(0);
    }
    if (chunks.length === 1) {
        return chunks[0]!;
    }
    return Buffer.concat(chunks);
}

/** Duration of PCM buffer in milliseconds (16-bit mono). */
export function pcmDurationMs(pcm: Buffer, sampleRate: number = UPLINK_SAMPLE_RATE): number {
    return (pcm.length / 2 / sampleRate) * 1000;
}

/** Number of bytes for a given duration at 16-bit mono. */
export function pcmBytesForDurationMs(durationMs: number, sampleRate: number = UPLINK_SAMPLE_RATE): number {
    return Math.floor((durationMs / 1000) * sampleRate) * 2;
}

/** Trim or left-pad PCM to exactly targetBytes (s16le). */
export function fitPcmWindow(pcm: Buffer, targetBytes: number): Buffer {
    if (pcm.length === targetBytes) {
        return pcm;
    }
    if (pcm.length > targetBytes) {
        return pcm.subarray(pcm.length - targetBytes);
    }
    const padded = Buffer.alloc(targetBytes);
    pcm.copy(padded, targetBytes - pcm.length);
    return padded;
}
