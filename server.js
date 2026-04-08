
Copy

const express    = require('express');
const { WebSocketServer } = require('ws');
const mqtt       = require('mqtt');
const http       = require('http');
const path       = require('path');
 
const app    = express();
const server = http.createServer(app);
 
// ── Static files (dashboard HTML served from /public) ────────────
app.use(express.static(path.join(__dirname, 'public')));
 
// ── Health / keep-alive endpoint ─────────────────────────────────
// Render free tier sleeps after 15 min of inactivity.
// Point UptimeRobot (or any free pinger) at /ping every 5 minutes
// to keep the server permanently awake.
app.get('/health', (req, res) => res.send('OK'));
app.get('/ping',   (req, res) => res.send('ok'));
 
// ── WebSocket server (noServer = we handle the upgrade manually) ──
const wss = new WebSocketServer({ noServer: true });
 
// ── Client sets ───────────────────────────────────────────────────
let browserClients   = new Set();   // dashboard browsers
let audioClients     = new Set();   // browsers listening to /audio
const videoClients   = new Set();   // browsers listening to /video
let esp32VideoSocket = null;        // single ESP32 video producer
let esp32AudioSocket = null;        // single ESP32 audio producer
 
// ── Latest sensor state (sent to newly-connected browsers) ────────
let latestData = {
    temperature: null,
    breathing:   null,
    heartrate:   null,
    alerts:      []
};
 
// ── HTTP → WebSocket upgrade routing ─────────────────────────────
// Each path is routed to the shared wss and tagged with a 'type'
// string so the connection handler knows what to do with it.
server.on('upgrade', (request, socket, head) => {
    const pathname = new URL(
        request.url,
        `http://${request.headers.host}`
    ).pathname;
 
    if (pathname === '/' || pathname === '/dashboard') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request, 'dashboard');
        });
    } else if (pathname === '/audio') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request, 'audio');
        });
    } else if (pathname === '/video') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request, 'video');
        });
    } else {
        // Unknown path — refuse the upgrade cleanly
        socket.destroy();
    }
});
 
// ── WebSocket connection handler ──────────────────────────────────
wss.on('connection', (ws, request, type) => {
 
    // ── Dashboard browser ────────────────────────────────────────
    if (type === 'dashboard') {
        console.log('[DASH] Browser connected.');
        browserClients.add(ws);
 
        // Send current sensor state immediately so the page
        // shows data even if it connects between MQTT publishes.
        ws.send(JSON.stringify({ type: 'state', data: latestData }));
 
        ws.on('close', () => {
            browserClients.delete(ws);
            console.log('[DASH] Browser disconnected.');
        });
    }
 
    // ── Audio ────────────────────────────────────────────────────
    // The ESP32 connects first and sends binary PCM frames.
    // Browser clients connect second and receive those frames.
    // We distinguish ESP32 from browser by User-Agent: ESP32's
    // WiFiClientSecure sends no User-Agent header.
    else if (type === 'audio') {
        const ua = (request.headers['user-agent'] || '');
        const isESP32 = !ua.includes('Mozilla');
 
        if (isESP32) {
            esp32AudioSocket = ws;
            console.log('[AUDIO] ESP32 producer connected.');
 
            ws.on('message', (data) => {
                // Relay raw binary PCM to all browser audio listeners.
                // The dashboard's Web Audio API decodes Int16 PCM directly —
                // sending as binary avoids the JSON serialisation overhead
                // that the previous version incurred (Array.from Int16Array).
                audioClients.forEach(client => {
                    if (client.readyState === 1) client.send(data);
                });
 
                // Also relay to dashboard clients so the existing
                // dashboard audio panel continues to work.
                browserClients.forEach(client => {
                    if (client.readyState === 1) client.send(data);
                });
            });
 
            ws.on('close', () => {
                esp32AudioSocket = null;
                console.log('[AUDIO] ESP32 producer disconnected.');
            });
 
        } else {
            audioClients.add(ws);
            console.log(`[AUDIO] Browser listener connected (${audioClients.size} total).`);
 
            ws.on('close', () => {
                audioClients.delete(ws);
            });
        }
    }
 
    // ── Video ────────────────────────────────────────────────────
    // Same pattern as audio. ESP32 sends binary JPEG frames.
    // Each message = one complete JPEG — no reassembly needed.
    // Browser receives frames and sets img.src to a Blob URL.
    else if (type === 'video') {
        const ua = (request.headers['user-agent'] || '');
        const isESP32 = !ua.includes('Mozilla');
 
        if (isESP32) {
            esp32VideoSocket = ws;
            console.log('[VIDEO] ESP32 producer connected.');
 
            ws.on('message', (data) => {
                videoClients.forEach(client => {
                    if (client.readyState === 1) client.send(data);
                });
            });
 
            ws.on('close', () => {
                esp32VideoSocket = null;
                console.log('[VIDEO] ESP32 producer disconnected.');
            });
 
        } else {
            videoClients.add(ws);
            console.log(`[VIDEO] Browser viewer connected (${videoClients.size} total).`);
 
            ws.on('close', () => {
                videoClients.delete(ws);
            });
        }
    }
});
 
// ── MQTT — HiveMQ Cloud ───────────────────────────────────────────
const mqttClient = mqtt.connect(
    'mqtts://55bf30b3bf6c4d7388f56f89e23855bd.s1.eu.hivemq.cloud',
    { port: 8883, username: 'BabyVitalsSystem', password: 'Password1234' }
);
 
mqttClient.on('connect', () => {
    console.log('[MQTT] Connected to HiveMQ Cloud.');
    mqttClient.subscribe([
        'babymonitor/temperature',
        'babymonitor/radar',
        'babymonitor/alerts',
        'babymonitor/status'
    ]);
});
 
mqttClient.on('message', (topic, message) => {
    try {
        const payload = JSON.parse(message.toString());
 
        // Update cached state
        if (topic === 'babymonitor/temperature') {
            latestData.temperature = payload;
        }
        if (topic === 'babymonitor/radar') {
            latestData.breathing = payload.breathing ?? null;
            latestData.heartrate = payload.heartrate ?? null;
        }
        if (topic === 'babymonitor/alerts') {
            latestData.alerts.unshift({ ...payload, topic });
            if (latestData.alerts.length > 20) latestData.alerts.pop();
        }
 
        // Broadcast to all connected dashboard browsers
        const msg = JSON.stringify({ type: 'sensor', topic, data: payload });
        browserClients.forEach(client => {
            if (client.readyState === 1) client.send(msg);
        });
 
    } catch (e) {
        console.error('[MQTT] Parse error:', e);
    }
});
 
mqttClient.on('error', (err) => {
    console.error('[MQTT] Connection error:', err.message);
});
 
// ── Start HTTP server ─────────────────────────────────────────────
const PORT = process.env.PORT || 10000;
server.listen(PORT, () => {
    console.log(`[SERVER] Running on port ${PORT}`);
    console.log(`[SERVER] WebSocket paths: /dashboard  /audio  /video`);
});
 
