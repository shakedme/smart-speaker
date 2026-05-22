/**
 * Offline smoke test for the LiveKit wakeword worker.
 *
 * Usage:
 *   WAKEWORD_MODEL_PATH=... node dist/scripts/test-wakeword-offline.js [pcm_or_wav_file]
 *
 * Without a file, sends synthetic silence and verifies the worker responds.
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

async function main(): Promise<void> {
    const fileArg = process.argv[2];
    const worker = new WakewordWorkerClient(WakewordWorkerClient.resolveDefaultModelPath());

    await worker.start();
    console.log('Worker started');

    const pcm = fileArg
        ? loadPcmFromFile(path.resolve(fileArg))
        : Buffer.alloc(pcmBytesForDurationMs(2000));

    const windowBytes = pcmBytesForDurationMs(2000);
    const window = pcm.length >= windowBytes
        ? pcm.subarray(pcm.length - windowBytes)
        : Buffer.concat([Buffer.alloc(windowBytes - pcm.length), pcm]);

    const result = await worker.predict(window);
    const threshold = loadWakewordThreshold();
    console.log(`Score: ${result.score.toFixed(4)} (model=${result.model})`);
    console.log(`Threshold: ${threshold}`);
    console.log(`Detected: ${result.score >= threshold}`);

    worker.shutdown();
}

main().catch((err) => {
    console.error(err);
    process.exit(1);
});
