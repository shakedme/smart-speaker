import {UPLINK_SAMPLE_RATE} from './pcm-utils';
import {decodeImaAdpcmBlockToPcm} from '../ima-adpcm';

export const UPLINK_FORMAT_PCM = 0;
export const UPLINK_FORMAT_IMA_ADPCM = 1;
export const UPLINK_FORMAT_FRAMED = 2;

export const UPLINK_FRAME_TYPE_MIC = 1;
export const UPLINK_FRAME_TYPE_PLAYBACK_REF = 2;
export const UPLINK_SAMPLE_FORMAT_INT16 = 1;
export const UPLINK_SAMPLE_FORMAT_IMA_ADPCM_BLOCK = 2;

export const UPLINK_FRAMED_HEADER_SIZE = 24;
export const AEC3_FRAME_SAMPLES = 160; // 10 ms @ 16 kHz
export const AEC3_FRAME_BYTES = AEC3_FRAME_SAMPLES * 2;

export interface ParsedUplinkFrame {
    frameType: number;
    sequence: number;
    timestampUs: number;
    sampleRate: number;
    channelCount: number;
    numSamples: number;
    pcm: Buffer;
}

export function isFramedUplink(data: Buffer): boolean {
    return data.length > 0 && data[0] === UPLINK_FORMAT_FRAMED;
}

export function parseFramedUplink(data: Buffer): ParsedUplinkFrame | null {
    if (!isFramedUplink(data)) {
        return null;
    }

    const minSize = 1 + UPLINK_FRAMED_HEADER_SIZE + 2;
    if (data.length < minSize) {
        return null;
    }

    const view = data.subarray(1);
    const frameType = view.readUInt8(0);
    const sampleFormat = view.readUInt8(1);
    const channelCount = view.readUInt16LE(2);
    const sampleRate = view.readUInt32LE(4);
    const sequence = view.readUInt32LE(8);
    const timestampUs = Number(view.readBigInt64LE(12));
    const numSamples = view.readUInt32LE(20);

    if (
        (sampleFormat !== UPLINK_SAMPLE_FORMAT_INT16 &&
            sampleFormat !== UPLINK_SAMPLE_FORMAT_IMA_ADPCM_BLOCK) ||
        channelCount === 0 ||
        numSamples === 0
    ) {
        return null;
    }

    const sampleCount = numSamples * channelCount;
    const payloadBytes = sampleFormat === UPLINK_SAMPLE_FORMAT_IMA_ADPCM_BLOCK
        ? 4 + Math.ceil(Math.max(0, sampleCount - 1) / 2)
        : sampleCount * 2;
    const expected = 1 + UPLINK_FRAMED_HEADER_SIZE + payloadBytes;
    if (data.length < expected) {
        return null;
    }

    const payload = data.subarray(1 + UPLINK_FRAMED_HEADER_SIZE, expected);
    const pcm = sampleFormat === UPLINK_SAMPLE_FORMAT_IMA_ADPCM_BLOCK
        ? decodeImaAdpcmBlockToPcm(payload, sampleCount)
        : payload;
    if (!pcm) {
        return null;
    }
    if (pcm.length % 2 !== 0) {
        return null;
    }

    return {
        frameType,
        sequence,
        timestampUs,
        sampleRate,
        channelCount,
        numSamples,
        pcm,
    };
}

/** Split a 20 ms (or larger) framed packet into 10 ms AEC3-compatible frames. */
export function splitAec3Frames(frame: ParsedUplinkFrame): ParsedUplinkFrame[] {
    if (frame.sampleRate !== UPLINK_SAMPLE_RATE || frame.channelCount !== 1) {
        return [frame];
    }

    const samplesPerFrame = AEC3_FRAME_SAMPLES;
    const totalSamples = frame.pcm.length / 2;
    if (totalSamples <= samplesPerFrame) {
        return [frame];
    }

    const out: ParsedUplinkFrame[] = [];
    for (let offset = 0; offset < totalSamples; offset += samplesPerFrame) {
        const count = Math.min(samplesPerFrame, totalSamples - offset);
        out.push({
            frameType: frame.frameType,
            sequence: frame.sequence,
            timestampUs: frame.timestampUs + Math.round((offset * 1_000_000) / frame.sampleRate),
            sampleRate: frame.sampleRate,
            channelCount: frame.channelCount,
            numSamples: count,
            pcm: frame.pcm.subarray(offset * 2, (offset + count) * 2),
        });
    }
    return out;
}
