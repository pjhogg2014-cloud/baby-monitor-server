const express = require('express');
const { WebSocketServer } = require('ws');
const mqtt = require('mqtt');
const http = require('http');
const path = require('path');

const app = express();
const server = http.createServer(app);

app.use(express.static(path.join(__dirname, 'public')));

// ---- WebSocket servers ----
// One for audio input from ESP32, one for browser dashboards
const audioServer   = new WebSocketServer({ server, path: '/audio' });
const browserServer = new WebSocketServer({ server, path: '/dashboard' });

let browserClients = new Set();
let latestData = {
    temperature: null, breathing: null,
    heartrate: null, alerts: []
};

// Track browser dashboard connections
browserServer.on('connection', (ws) => {
    console.log('Browser dashboard connected.');
    browserClients.add(ws);
    // Send latest known state immediately on connect
    ws.send(JSON.stringify({ type: 'state', data: latestData }));
    ws.on('close', () => browserClients.delete(ws));
});

// Relay audio from ESP32 to all browsers
audioServer.on('connection', (ws) => {
    console.log('ESP32 audio stream connected.');
    ws.on('message', (data) => {
        browserClients.forEach(client => {
            if (client.readyState === 1) {
                client.send(JSON.stringify({
                    type: 'audio',
                    data: Array.from(new Int16Array(data))
                }));
            }
        });
    });
    ws.on('close', () => console.log('ESP32 audio disconnected.'));
});
server.on('upgrade', (request, socket, head) => {
    const pathname = request.url;
    if (pathname === '/dashboard') {
        browserServer.handleUpgrade(request, socket, head, (ws) => {
            browserServer.emit('connection', ws, request);
        });
    } else if (pathname === '/audio') {
        audioServer.handleUpgrade(request, socket, head, (ws) => {
            audioServer.emit('connection', ws, request);
        });
    } else {
        socket.destroy();
    }
});
// ---- MQTT subscriber ----
const mqttClient = mqtt.connect('mqtts://55bf30b3bf6c4d7388f56f89e23855bd.s1.eu.hivemq.cloud', {
    port: 8883,
    username: 'BabyVitalsSystem',
    password: 'Password1234'
});

mqttClient.on('connect', () => {
    console.log('MQTT connected.');
    mqttClient.subscribe([
        'babymonitor/temperature',
        'babymonitor/breathing',
        'babymonitor/alerts',
        'babymonitor/status'
    ]);
});

mqttClient.on('message', (topic, message) => {
    try {
        const payload = JSON.parse(message.toString());
        // Update latest state
        if (topic === 'babymonitor/temperature')
            latestData.temperature = payload;
        if (topic === 'babymonitor/breathing')
            latestData.breathing = payload;
        if (topic === 'babymonitor/alerts')
            latestData.alerts.unshift({ ...payload, topic });

        // Forward to all browser dashboards
        const msg = JSON.stringify({ type: 'sensor', topic, data: payload });
        browserClients.forEach(client => {
            if (client.readyState === 1) client.send(msg);
        });
    } catch (e) { console.error('MQTT parse error:', e); }
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log(`Server running on port ${PORT}`));
