// server.js  (Single file version)
const express = require('express');
const { WebSocketServer } = require('ws');
const mqtt = require('mqtt');
const http = require('http');
const path = require('path');
 
const app = express();
const server = http.createServer(app);
 
app.use(express.static(path.join(__dirname, 'public')));
 
// Health check
app.get('/', (req, res) => {
    res.send('Baby Monitor Server is running ✅');
});
 
// ====================== WEBSOCKET SETUP ======================
const wss = new WebSocketServer({ noServer: true });
 
server.on('upgrade', (request, socket, head) => {
const pathname = new URL(request.url, `http://${request.headers.host}`).pathname;
 
    if (pathname === '/' || pathname === '/dashboard') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request, 'dashboard');
        });
    } else if (pathname === '/audio') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request, 'audio');
        });
    } else {
        socket.destroy();
    }
});
 
let browserClients = new Set();
let latestData = {
    temperature: null,
    breathing: null,
    heartrate: null,
    alerts: []
};
 
wss.on('connection', (ws, request, type) => {
    if (type === 'dashboard') {
        console.log('✅ Browser dashboard connected');
        browserClients.add(ws);
 
        ws.send(JSON.stringify({ type: 'state', data: latestData }));
 
        ws.on('close', () => {
            browserClients.delete(ws);
            console.log('Browser dashboard disconnected');
        });
    }
    else if (type === 'audio') {
        console.log('✅ ESP32 audio stream connected');
 
        ws.on('message', (data) => {
            const audioPayload = Array.from(new Int16Array(data));
            browserClients.forEach(client => {
                if (client.readyState === 1) {
                    client.send(JSON.stringify({ type: 'audio', data: audioPayload }));
                }
            });
        });
 
        ws.on('close', () => console.log('ESP32 audio disconnected'));
    }
});
 
// ====================== MQTT ======================
const mqttClient = mqtt.connect('mqtts://55bf30b3bf6c4d7388f56f89e23855bd.s1.eu.hivemq.cloud', {
    port: 8883,
    username: 'BabyVitalsSystem',
    password: 'Password1234'
});
 
mqttClient.on('connect', () => {
    console.log('MQTT connected.');
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
 
        if (topic === 'babymonitor/temperature') {
            latestData.temperature = payload;
        }
        if (topic === 'babymonitor/radar') {
            latestData.breathing = payload.breathing || payload;
            latestData.heartrate = payload.heartrate || null;
        }
        if (topic === 'babymonitor/alerts') {
            latestData.alerts.unshift({ ...payload, topic });
            if (latestData.alerts.length > 20) latestData.alerts.pop();
        }
 
        const msg = JSON.stringify({ type: 'sensor', topic, data: payload });
 
        browserClients.forEach(client => {
            if (client.readyState === 1) client.send(msg);
        });
    } catch (e) {
        console.error('MQTT parse error:', e);
    }
});
 
// ====================== START SERVER ======================
const PORT = process.env.PORT || 10000;
server.listen(PORT, () => {
    console.log(Server running on port ${PORT});
    console.log(WebSocket ready → wss://baby-monitor-server-0st8.onrender.com);
});
