import express from 'express';
import { WebSocketServer, RawData } from 'ws';
import http from 'http';
import path from 'path';
import { config } from 'dotenv';

import { WebSocketHandler } from './websocket-handler';

config();

const app = express();
const server = http.createServer(app);

// Serve static files from public directory
app.use(express.static(path.join(__dirname, '../public')));

// WebSocket server for voice streaming
const wss = new WebSocketServer({ 
    server,
    path: '/voice'
});

// Basic health check endpoint
app.get('/health', (req, res) => {
    res.json({ 
        status: 'ok', 
        timestamp: new Date().toISOString(),
        connections: wss.clients.size
    });
});

// WebSocket connection handling
wss.on('connection', (ws, request) => {
    const remoteAddress = request.socket.remoteAddress || 'unknown';
    console.log(`New WebSocket connection from ${remoteAddress}`);
    
    const handler = new WebSocketHandler(ws);
    
    ws.on('message', (data: RawData, isBinary: boolean) => {
        // Convert RawData to Buffer for consistent handling
        const buffer = Buffer.isBuffer(data) ? data : Buffer.from(data as ArrayBuffer);
        handler.handleMessage(buffer, isBinary);
    });
    
    ws.on('close', (code, reason) => {
        console.log(`WebSocket connection closed: ${code} ${reason.toString()}`);
        handler.cleanup();
    });
    
    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
        handler.cleanup();
    });
});

const PORT = process.env.PORT ? parseInt(process.env.PORT, 10) : 8080;

server.listen(PORT, () => {
    console.log(`🎤 BT Speaker Backend Server running on port ${PORT}`);
    console.log(`🌐 Frontend: http://localhost:${PORT}`);
    console.log(`📡 WebSocket endpoint: ws://localhost:${PORT}/voice`);
    console.log(`🔍 Health check: http://localhost:${PORT}/health`);
}); 