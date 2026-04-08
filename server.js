const express    = require('express');
const { WebSocketServer } = require('ws');
const mqtt       = require('mqtt');
const http       = require('http');
const path       = require('path');

const app    = express();
const server = http.createServer(app);

app.use(express.static(path.join(__dirname, 'public')));
app.get('/health', (req, res) => res.send('OK'));
app.get('/ping',   (req, res) => res.send('ok'));

const wss = new WebSocketServer({ noServer: true });

let browserClients   = new Set();
let audioClients     = new Set();
const videoClients   = new Set();
let esp32VideoSocket = null;
let esp32AudioSocket = null;

let latestData = {
    temperature: null,
    breathing:   null,
    heartrate:   null,
    alerts:      []
};

server.on('upgrade', (request, socket, head) => {
    const pathname = new URL(request.url, `http://${request.headers.host}`).pathname;
    if (pathname === '/' || pathname === '/dashboard') {
        wss.handleUpgrade(request, socket, head, (ws) => wss.emit('connection', ws, request, 'dashboard'));
    } else if (pathname === '/audio') {
        wss.handleUpgrade(request, socket, head, (ws) => wss.emit('connection', ws, request, 'audio'));
    } else if (pathname === '/video') {
        wss.handleUpgrade(request, socket, head, (ws) => wss.emit('connection', ws, request, 'video'));
    } else {
        socket.destroy();
    }
});

wss.on('connection', (ws, request, type) => {

    if (type === 'dashboard') {
        console.log('[DASH] Browser connected.');
        browserClients.add(ws);
        ws.send(JSON.stringify({ type: 'state', data: latestData }));
        ws.on('close', () => { browserClients.delete(ws); console.log('[DASH] Browser disconnected.'); });
    }

    else if (type === 'audio') {
        const isESP32 = !(request.headers['user-agent'] || '').includes('Mozilla');
        if (isESP32) {
            esp32AudioSocket = ws;
            console.log('[AUDIO] ESP32 producer connected.');
            ws.on('message', (data) => {
                // BUG 4 FIX: relay ONLY to /audio subscribers, not browserClients.
                // Sending binary PCM to browserClients (/dashboard) caused binary
                // frames to arrive on the JSON WebSocket where JSON.parse() threw
                // silently, breaking the audio visualiser.
                audioClients.forEach(c => { if (c.readyState === 1) c.send(data); });
            });
            ws.on('close', () => { esp32AudioSocket = null; console.log('[AUDIO] ESP32 disconnected.'); });
        } else {
            audioClients.add(ws);
            console.log(`[AUDIO] Browser listener (${audioClients.size} total).`);
            ws.on('close', () => audioClients.delete(ws));
        }
    }

    else if (type === 'video') {
        const isESP32 = !(request.headers['user-agent'] || '').includes('Mozilla');
        if (isESP32) {
            esp32VideoSocket = ws;
            console.log('[VIDEO] ESP32 producer connected.');
            ws.on('message', (data) => {
                videoClients.forEach(c => { if (c.readyState === 1) c.send(data); });
            });
            ws.on('close', () => { esp32VideoSocket = null; console.log('[VIDEO] ESP32 disconnected.'); });
        } else {
            videoClients.add(ws);
            console.log(`[VIDEO] Browser viewer (${videoClients.size} total).`);
            ws.on('close', () => videoClients.delete(ws));
        }
    }
});

const mqttBroker = mqtt.connect(
    'mqtts://55bf30b3bf6c4d7388f56f89e23855bd.s1.eu.hivemq.cloud',
    { port: 8883, username: 'BabyVitalsSystem', password: 'Password1234' }
);

mqttBroker.on('connect', () => {
    console.log('[MQTT] Connected to HiveMQ Cloud.');
    mqttBroker.subscribe(['babymonitor/temperature','babymonitor/radar','babymonitor/alerts','babymonitor/status']);
});

mqttBroker.on('message', (topic, message) => {
    try {
        const payload = JSON.parse(message.toString());
        if (topic === 'babymonitor/temperature') latestData.temperature = payload;
        if (topic === 'babymonitor/radar') {
            latestData.breathing = payload.breathing ?? null;
            latestData.heartrate = payload.heartrate ?? null;
        }
        if (topic === 'babymonitor/alerts') {
            latestData.alerts.unshift({ ...payload, topic });
            if (latestData.alerts.length > 20) latestData.alerts.pop();
        }
        // JSON sensor messages only — no binary mixed in
        const msg = JSON.stringify({ type: 'sensor', topic, data: payload });
        browserClients.forEach(c => { if (c.readyState === 1) c.send(msg); });
    } catch (e) {
        console.error('[MQTT] Parse error:', e);
    }
});

mqttBroker.on('error', (err) => console.error('[MQTT] Error:', err.message));

const PORT = process.env.PORT || 10000;
server.listen(PORT, () => {
    console.log(`[SERVER] Port ${PORT} | paths: /dashboard /audio /video`);
});
