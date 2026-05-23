/**
 * Offline smoke test for the LiveKit wakeword worker.
 *
 * Usage:
 *   node dist/scripts/test-wakeword-offline.js [pcm_or_wav_file_or_directory]
 *
 * Without an argument, sends synthetic silence and verifies the worker responds.
 */

import * as fs from 'node:fs';
import path from 'node:path';
import {WakewordWorkerClient} from '../voice/wakeword-worker-client';
import {pcmBytesForDurationMs} from '../audio/pcm-utils';
import {loadWakewordThreshold} from '../voice/wakeword-config';

function loadPcmFromFile(filePath: string): Buffer {
    const ext = path.extname(filePath).toLowerCase();
    const raw = fs.readFileSync(filePath);
    if (ext === '.pcm') {
        return raw;
    }
    if (ext === '.wav' && raw.length > 44) {
        return raw.subarray(44);
    }
    throw new Error(`Unsupported audio file: ${filePath} (use .pcm or .wav)`);
}

function listAudioFiles(inputPath: string): string[] {
    const stat = fs.statSync(inputPath);
    if (stat.isFile()) {
        return [inputPath];
    }
    if (!stat.isDirectory()) {
        throw new Error(`Not a file or directory: ${inputPath}`);
    }

    return fs.readdirSync(inputPath)
        .filter((file) => file.endsWith('.wav') || file.endsWith('.pcm'))
        .map((file) => path.join(inputPath, file))
        .sort();
}

function windowFromPcm(pcm: Buffer): Buffer {
    const windowBytes = pcmBytesForDurationMs(2000);
    return pcm.length >= windowBytes
        ? pcm.subarray(pcm.length - windowBytes)
        : Buffer.concat([Buffer.alloc(windowBytes - pcm.length), pcm]);
}

async function scoreFile(
    worker: WakewordWorkerClient,
    filePath: string,
    threshold: number,
): Promise<{file: string; score: number; detected: boolean; predictMs: number}> {
    const pcm = loadPcmFromFile(filePath);
    const window = windowFromPcm(pcm);
    const result = await worker.predict(window);
    return {
        file: path.basename(filePath),
        score: result.score,
        detected: result.score >= threshold,
        predictMs: result.predictMs,
    };
}

async function main(): Promise<void> {
    const inputArg = process.argv[2];
    const worker = new WakewordWorkerClient(WakewordWorkerClient.resolveDefaultModelPath());

    await worker.start();
    console.log('Worker started');

    const threshold = loadWakewordThreshold();
    console.log(`Threshold: ${threshold}`);

    if (!inputArg) {
        const window = windowFromPcm(Buffer.alloc(pcmBytesForDurationMs(2000)));
        const result = await worker.predict(window);
        console.log(`Score: ${result.score.toFixed(4)} (model=${result.model}, predict_ms=${result.predictMs.toFixed(1)})`);
        console.log(`Detected: ${result.score >= threshold}`);
        worker.shutdown();
        return;
    }

    const files = listAudioFiles(path.resolve(inputArg));
    if (files.length === 0) {
        throw new Error(`No .wav or .pcm files found at ${inputArg}`);
    }

    const results = [];
    for (const file of files) {
        results.push(await scoreFile(worker, file, threshold));
    }

    for (const {file, score, detected, predictMs} of results) {
        console.log(`${detected ? 'PASS' : 'FAIL'} ${score.toFixed(4)} predict_ms=${predictMs.toFixed(1)} ${file}`);
    }

    const detected = results.filter((r) => r.detected).length;
    const scores = results.map((r) => r.score);
    const avg = scores.reduce((a, b) => a + b, 0) / scores.length;
    const min = Math.min(...scores);
    const max = Math.max(...scores);
    const predictTimes = results.map((r) => r.predictMs);
    const avgPredict =
        predictTimes.reduce((a, b) => a + b, 0) / Math.max(1, predictTimes.length);

    console.log('');
    console.log(`Clips: ${results.length}  Detected: ${detected}/${results.length} (${((detected / results.length) * 100).toFixed(1)}%)`);
    console.log(`Score min/avg/max: ${min.toFixed(4)} / ${avg.toFixed(4)} / ${max.toFixed(4)}`);
    console.log(`predict_ms avg: ${avgPredict.toFixed(1)} (tune WAKEWORD_INFERENCE_INTERVAL_MS >= p95-ish)`);

    worker.shutdown();
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});
