import express from 'express';
import {createServer} from 'http';
import {WebSocketServer} from 'ws';
import {LiveWebSocketHandler} from './live-websocket-handler';
import {
    getSharedWakewordWorker,
    shutdownSharedWakewordWorker,
} from './voice/wakeword-worker-client';
import {loadWakewordEvalMetrics, loadWakewordThreshold} from './voice/wakeword-config';
import path from 'path';

import dotenv from 'dotenv';

dotenv.config();

const app = express();
const port = process.env.PORT || 8080;

app.use(express.static(path.join(__dirname, '../public')));

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, '../public/index.html'));
});

const server = createServer(app);

const wss = new WebSocketServer({
    server,
    path: '/voice'
});

console.log('BT Speaker Live Voice Server');
console.log('Environment:', process.env.NODE_ENV || 'development');

const wakewordWorker = getSharedWakewordWorker();
const activeHandlers = new Set<LiveWebSocketHandler>();
let shuttingDown = false;

async function shutdown(signal: string): Promise<void> {
    if (shuttingDown) {
        return;
    }
    shuttingDown = true;
    console.log(`${signal} received, shutting down gracefully`);

    await Promise.allSettled([...activeHandlers].map((handler) => handler.cleanup()));
    activeHandlers.clear();
    shutdownSharedWakewordWorker();
    wss.close();
    server.close();
    process.exit(0);
}

async function boot(): Promise<void> {
    const threshold = loadWakewordThreshold();
    const evalMetrics = loadWakewordEvalMetrics();
    console.log(`Wake word threshold: ${threshold}${process.env.WAKEWORD_THRESHOLD ? ' (from env)' : ' (from eval/default)'}`);
    if (evalMetrics) {
        console.log(
            `Wake word eval: recall=${((evalMetrics.recall ?? 0) * 100).toFixed(1)}% ` +
            `fpph=${evalMetrics.fpph ?? 'n/a'} aut=${evalMetrics.aut ?? 'n/a'}`,
        );
    } else {
        console.warn('No okay_rufus_eval.json found. Train the prod model: npm run train:wakeword:prod');
    }

    try {
        await wakewordWorker.start();
        console.log('Wake word worker ready');
    } catch (error) {
        console.error('Failed to start wake word worker:', error);
        console.error('Ensure livekit-wakeword is installed and WAKEWORD_MODEL_PATH points to a trained ONNX file.');
        process.exit(1);
    }

    wss.on('connection', (ws, request) => {
        const clientIP = request.socket.remoteAddress;
        request.socket.setNoDelay(true);
        console.log(`New WebSocket connection from ${clientIP}`);

        LiveWebSocketHandler.create(ws, wakewordWorker)
            .then((handler) => {
                activeHandlers.add(handler);

                ws.on('message', (data, isBinary) => {
                    try {
                        const buffer = Buffer.isBuffer(data) ? data : Buffer.from(data as Uint8Array);
                        handler.handleMessage(buffer, isBinary);
                    } catch (error) {
                        console.error('Error handling WebSocket message:', error);
                        ws.close(1011, 'Internal server error');
                    }
                });

                ws.on('close', async (code, reason) => {
                    console.log(`WebSocket connection closed: ${code} ${reason.toString()}`);
                    activeHandlers.delete(handler);
                    await handler.cleanup();
                });

                ws.on('error', (error) => {
                    console.error('WebSocket error:', error);
                });
            })
            .catch((error) => {
                console.error('Failed to create session handler:', error);
                ws.close(1011, 'Failed to initialize voice session');
            });
    });

    server.listen(Number(port), '0.0.0.0', () => {
        console.log(`Server running on http://localhost:${port}`);
        console.log(`WebSocket endpoint: ws://localhost:${port}/voice`);
        console.log(`Frontend: http://localhost:${port}`);
    });
}

void boot();

process.on('SIGTERM', () => {
    void shutdown('SIGTERM');
});

process.on('SIGINT', () => {
    void shutdown('SIGINT');
});
