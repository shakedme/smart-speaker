import * as fs from 'node:fs';
import path from 'node:path';
import {pcmBytesForDurationMs, UPLINK_SAMPLE_RATE} from '../audio/pcm-utils';

interface Segment {
    startByte: number;
    endByte: number;
    peakRms: number;
}

interface SegmentOptions {
    frameMs: number;
    minSpeechMs: number;
    mergeGapMs: number;
    prePadMs: number;
    postPadMs: number;
    clipMs: number;
    threshold?: number;
}

const DEFAULT_OPTIONS: SegmentOptions = {
    frameMs: 30,
    minSpeechMs: 250,
    mergeGapMs: 350,
    prePadMs: 350,
    postPadMs: 550,
    clipMs: 2000,
};

function parseArgs(): {input: string; outputDir: string; options: SegmentOptions} {
    const args = process.argv.slice(2);
    const input = args[0] ?? 'models/wakeword/captures/raw';
    const outputDir = args[1] ?? 'models/wakeword/captures/positive';
    const thresholdArg = args.find((arg) => arg.startsWith('--threshold='));
    const threshold = thresholdArg ? Number(thresholdArg.split('=')[1]) : undefined;
    const options: SegmentOptions = {...DEFAULT_OPTIONS};
    if (threshold !== undefined && Number.isFinite(threshold)) {
        options.threshold = threshold;
    }

    return {
        input: path.resolve(input),
        outputDir: path.resolve(outputDir),
        options,
    };
}

function listPcmFiles(input: string): string[] {
    const stat = fs.statSync(input);
    if (stat.isFile()) {
        return input.endsWith('.pcm') ? [input] : [];
    }

    return fs.readdirSync(input)
        .filter((file) => file.endsWith('.pcm'))
        .map((file) => path.join(input, file))
        .sort();
}

function frameRms(pcm: Buffer, startByte: number, frameBytes: number): number {
    const endByte = Math.min(startByte + frameBytes, pcm.length);
    let sumSquares = 0;
    let count = 0;

    for (let offset = startByte; offset + 1 < endByte; offset += 2) {
        const sample = pcm.readInt16LE(offset);
        sumSquares += sample * sample;
        count++;
    }

    return count > 0 ? Math.sqrt(sumSquares / count) : 0;
}

function percentile(values: number[], p: number): number {
    if (values.length === 0) {
        return 0;
    }
    const sorted = [...values].sort((a, b) => a - b);
    const idx = Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length));
    return sorted[idx] ?? 0;
}

function detectSegments(pcm: Buffer, options: SegmentOptions): Segment[] {
    const frameBytes = Math.max(2, pcmBytesForDurationMs(options.frameMs));
    const minSpeechBytes = pcmBytesForDurationMs(options.minSpeechMs);
    const mergeGapBytes = pcmBytesForDurationMs(options.mergeGapMs);
    const rmsValues: number[] = [];

    for (let start = 0; start < pcm.length; start += frameBytes) {
        rmsValues.push(frameRms(pcm, start, frameBytes));
    }

    const noiseFloor = percentile(rmsValues, 20);
    const threshold = options.threshold ?? Math.max(350, noiseFloor * 4);
    const rawSegments: Segment[] = [];
    let active: Segment | null = null;

    for (let i = 0; i < rmsValues.length; i++) {
        const rms = rmsValues[i] ?? 0;
        const startByte = i * frameBytes;
        const endByte = Math.min(startByte + frameBytes, pcm.length);
        if (rms >= threshold) {
            if (!active) {
                active = {startByte, endByte, peakRms: rms};
            } else {
                active.endByte = endByte;
                active.peakRms = Math.max(active.peakRms, rms);
            }
        } else if (active) {
            if (active.endByte - active.startByte >= minSpeechBytes) {
                rawSegments.push(active);
            }
            active = null;
        }
    }

    if (active && active.endByte - active.startByte >= minSpeechBytes) {
        rawSegments.push(active);
    }

    const merged: Segment[] = [];
    for (const segment of rawSegments) {
        const previous = merged[merged.length - 1];
        if (previous && segment.startByte - previous.endByte <= mergeGapBytes) {
            previous.endByte = segment.endByte;
            previous.peakRms = Math.max(previous.peakRms, segment.peakRms);
        } else {
            merged.push({...segment});
        }
    }

    return merged;
}

function makeClip(pcm: Buffer, segment: Segment, options: SegmentOptions): Buffer {
    const prePadBytes = pcmBytesForDurationMs(options.prePadMs);
    const postPadBytes = pcmBytesForDurationMs(options.postPadMs);
    const clipBytes = pcmBytesForDurationMs(options.clipMs);

    let start = Math.max(0, segment.startByte - prePadBytes);
    let end = Math.min(pcm.length, segment.endByte + postPadBytes);
    const segmentBytes = end - start;

    if (segmentBytes < clipBytes) {
        const missing = clipBytes - segmentBytes;
        start = Math.max(0, start - Math.floor(missing / 2));
        end = Math.min(pcm.length, end + Math.ceil(missing / 2));
    }

    return fitClip(pcm.subarray(start, end), clipBytes);
}

function fitClip(pcm: Buffer, clipBytes: number): Buffer {
    if (pcm.length === clipBytes) {
        return pcm;
    }
    if (pcm.length > clipBytes) {
        const start = Math.floor((pcm.length - clipBytes) / 2);
        return pcm.subarray(start, start + clipBytes);
    }

    const out = Buffer.alloc(clipBytes);
    pcm.copy(out, Math.floor((clipBytes - pcm.length) / 2));
    return out;
}

function createWavBuffer(pcmData: Buffer): Buffer {
    const numChannels = 1;
    const bitsPerSample = 16;
    const byteRate = UPLINK_SAMPLE_RATE * numChannels * (bitsPerSample / 8);
    const blockAlign = numChannels * (bitsPerSample / 8);
    const dataSize = pcmData.length;
    const buffer = Buffer.alloc(44);

    buffer.write('RIFF', 0);
    buffer.writeUInt32LE(36 + dataSize, 4);
    buffer.write('WAVE', 8);
    buffer.write('fmt ', 12);
    buffer.writeUInt32LE(16, 16);
    buffer.writeUInt16LE(1, 20);
    buffer.writeUInt16LE(numChannels, 22);
    buffer.writeUInt32LE(UPLINK_SAMPLE_RATE, 24);
    buffer.writeUInt32LE(byteRate, 28);
    buffer.writeUInt16LE(blockAlign, 32);
    buffer.writeUInt16LE(bitsPerSample, 34);
    buffer.write('data', 36);
    buffer.writeUInt32LE(dataSize, 40);

    return Buffer.concat([buffer, pcmData]);
}

function main(): void {
    const {input, outputDir, options} = parseArgs();
    fs.mkdirSync(outputDir, {recursive: true});

    const files = listPcmFiles(input);
    if (files.length === 0) {
        throw new Error(`No .pcm captures found at ${input}`);
    }

    let total = 0;
    for (const file of files) {
        const pcm = fs.readFileSync(file);
        const segments = detectSegments(pcm, options);
        const base = path.basename(file, '.pcm');

        segments.forEach((segment, index) => {
            const clip = makeClip(pcm, segment, options);
            const outPath = path.join(outputDir, `${base}_wake_${String(index + 1).padStart(3, '0')}.wav`);
            fs.writeFileSync(outPath, createWavBuffer(clip));
            total++;
        });

        console.log(`${path.basename(file)}: ${segments.length} clips`);
    }

    console.log(`Wrote ${total} clips to ${outputDir}`);
}

main();
