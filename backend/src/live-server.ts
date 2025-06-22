import express from 'express';
import {createServer} from 'http';
import {WebSocketServer} from 'ws';
import {LiveWebSocketHandler} from './live-websocket-handler';
import path from 'path';

// Load environment variables
import dotenv from 'dotenv';

dotenv.config();

const app = express();
const port = process.env.PORT || 8080;

// Serve static files from public directory (always relative to backend)
app.use(express.static(path.join(__dirname, '../public')));

// Serve index.html for root path
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, '../public/index.html'));
});

// Create HTTP server
const server = createServer(app);

// Create WebSocket server
const wss = new WebSocketServer({
    server,
    path: '/voice'
});

console.log('🎤 BT Speaker Live Voice Server');
console.log('📊 Environment:', process.env.NODE_ENV || 'development');

wss.on('connection', (ws, request) => {
    const clientIP = request.socket.remoteAddress;
    console.log(`🔗 New WebSocket connection from ${clientIP}`);

    // Create handler for this connection
    const handler = new LiveWebSocketHandler(ws);

    // Handle WebSocket messages
    ws.on('message', (data, isBinary) => {
        try {
            // Convert RawData to Buffer properly
            const buffer = Buffer.isBuffer(data) ? data : Buffer.from(data as Uint8Array);
            handler.handleMessage(buffer, isBinary);
        } catch (error) {
            console.error('❌ Error handling WebSocket message:', error);
            ws.close(1011, 'Internal server error');
        }
    });

    // Handle connection close
    ws.on('close', async (code, reason) => {
        console.log(`🔌 WebSocket connection closed: ${code} ${reason.toString()}`);
        await handler.cleanup();
    });

    // Handle connection error
    ws.on('error', (error) => {
        console.error('❌ WebSocket error:', error);
    });
});

// Start server
server.listen(Number(port), '0.0.0.0', () => {
    console.log(`🚀 Server running on http://localhost:${port}`);
    console.log(`📡 WebSocket endpoint: ws://localhost:${port}/voice`);
    console.log(`🌐 Frontend: http://localhost:${port}`);
    console.log('');
    console.log('💡 Open the frontend URL in your browser to test the Live API');
});

// Graceful shutdown
process.on('SIGTERM', () => {
    console.log('🛑 SIGINT received, shutting down gracefully');
    process.exit(0);
});

process.on('SIGINT', () => {
    console.log('🛑 SIGINT received, shutting down gracefully');
    process.exit(0);
});