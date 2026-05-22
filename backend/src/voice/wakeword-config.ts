import * as fs from 'node:fs';
import path from 'node:path';

export interface WakewordEvalMetrics {
    threshold?: number;
    optimal_threshold?: number;
    recall?: number;
    fpph?: number;
    aut?: number;
}

const DEFAULT_THRESHOLD = 0.68;

/** Resolve detection threshold: env override, then eval JSON, then default. */
export function loadWakewordThreshold(modelName: string = 'okay_rufus'): number {
    if (process.env.WAKEWORD_THRESHOLD) {
        return parseFloat(process.env.WAKEWORD_THRESHOLD);
    }

    const evalPath = path.resolve(
        process.cwd(),
        `models/wakeword/output/${modelName}/${modelName}_eval.json`,
    );
    if (!fs.existsSync(evalPath)) {
        return DEFAULT_THRESHOLD;
    }

    try {
        const metrics = JSON.parse(fs.readFileSync(evalPath, 'utf8')) as WakewordEvalMetrics;
        if (typeof metrics.threshold === 'number' && metrics.threshold > 0.05) {
            return metrics.threshold;
        }
        console.warn(
            `[WAKEWORD] Eval threshold ${metrics.threshold} looks invalid; using default ${DEFAULT_THRESHOLD}`,
        );
    } catch (error) {
        console.warn(`[WAKEWORD] Failed to read eval metrics from ${evalPath}: ${error}`);
    }

    return DEFAULT_THRESHOLD;
}

export function loadWakewordEvalMetrics(modelName: string = 'okay_rufus'): WakewordEvalMetrics | null {
    const evalPath = path.resolve(
        process.cwd(),
        `models/wakeword/output/${modelName}/${modelName}_eval.json`,
    );
    if (!fs.existsSync(evalPath)) {
        return null;
    }
    try {
        return JSON.parse(fs.readFileSync(evalPath, 'utf8')) as WakewordEvalMetrics;
    } catch {
        return null;
    }
}
