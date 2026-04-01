'use strict';

require('dotenv').config();

const express = require('express');
const cors    = require('cors');
const path    = require('path');
const mqtt    = require('mqtt');
const app     = express();
const PORT    = process.env.PORT || 3000;

// ============================================================
//  HiveMQ Config — matches firmware exactly
// ============================================================
const HIVEMQ_HOST     = process.env.HIVEMQ_HOST     || 'a0d0d0e9332a4d3db7516f6125f6e677.s1.eu.hivemq.cloud';
const HIVEMQ_PORT     = parseInt(process.env.HIVEMQ_PORT || '8883');
const HIVEMQ_USERNAME = process.env.HIVEMQ_USERNAME || 'Highbaylight';
const HIVEMQ_PASSWORD = process.env.HIVEMQ_PASSWORD || 'Naveen235623@@';
const HIVEMQ_CLIENT   = 'aipl-server-' + Math.random().toString(16).slice(2, 8);

// ============================================================
//  TOPIC HELPERS — must match firmware snprintf patterns exactly
//
//  Firmware publishes  : aipl/row/{R}/light/{L}/state      payload: ON | OFF
//  Firmware subscribes : aipl/row/{R}/light/{L}/command    payload: ON | OFF
//                        aipl/row/{R}/command              payload: ON | OFF
//                        aipl/all/command                  payload: ON | OFF
//
//  Server subscribes   : aipl/row/+/light/+/state  (wildcard, all 36 devices)
//  Server publishes    : command topics above
// ============================================================
const TOPIC_CMD_SINGLE = (r, l) => `aipl/row/${r}/light/${l}/command`;
const TOPIC_CMD_ROW    = (r)    => `aipl/row/${r}/command`;
const TOPIC_CMD_ALL    = ()     => `aipl/all/command`;
const STATE_WILDCARD   = 'aipl/row/+/light/+/state';

// ============================================================
//  IN-MEMORY GRID  [row 0-5][light 0-5]
//
//  *** Default: true (ON) ***
//  Because the firmware boots with light ON (fail-safe).
//  If the server defaults to false but ESP32 is physically ON,
//  the dashboard shows wrong state until the first MQTT message.
//  Defaulting to true avoids that visual glitch.
// ============================================================
const grid = Array.from({ length: 6 }, () => Array(6).fill(true));

// ============================================================
//  MQTT CLIENT
// ============================================================
const mqttClient = mqtt.connect(`mqtts://${HIVEMQ_HOST}:${HIVEMQ_PORT}`, {
  username:           HIVEMQ_USERNAME,
  password:           HIVEMQ_PASSWORD,
  clientId:           HIVEMQ_CLIENT,
  rejectUnauthorized: true,   // verify HiveMQ TLS cert
  reconnectPeriod:    3000,   // retry every 3s on disconnect
  keepalive:          60,
});

mqttClient.on('connect', () => {
  console.log('[MQTT] Connected to HiveMQ as', HIVEMQ_CLIENT);
  // Subscribe to ALL device state topics with wildcard
  mqttClient.subscribe(STATE_WILDCARD, { qos: 1 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error:', err.message);
    else     console.log('[MQTT] Subscribed to', STATE_WILDCARD);
  });
});

// ── Incoming state from each ESP32 ─────────────────────────
mqttClient.on('message', (topic, payload) => {
  // Expected topic: aipl/row/R/light/L/state
  // parts:          [0]  [1] [2][3]   [4][5]
  const parts = topic.split('/');
  if (
    parts.length === 6   &&
    parts[0] === 'aipl'  &&
    parts[1] === 'row'   &&
    parts[3] === 'light' &&
    parts[5] === 'state'
  ) {
    const row   = parseInt(parts[2], 10);
    const light = parseInt(parts[4], 10);
    const state = payload.toString().trim().toUpperCase() === 'ON';

    if (row >= 0 && row < 6 && light >= 0 && light < 6) {
      grid[row][light] = state;
      console.log(`[STATE] Row ${row+1} Light ${light+1} → ${state ? 'ON' : 'OFF'}`);
    }
  }
});

mqttClient.on('error',     (e) => console.error('[MQTT] Error:',      e.message));
mqttClient.on('reconnect', ()  => console.log  ('[MQTT] Reconnecting...'));
mqttClient.on('offline',   ()  => console.warn ('[MQTT] Went offline'));

// ── Publish helper ─────────────────────────────────────────
function publish(topic, payload) {
  return new Promise((resolve, reject) => {
    if (!mqttClient.connected)
      return reject(new Error('MQTT not connected'));
    mqttClient.publish(topic, payload, { qos: 1, retain: false }, (err) => {
      if (err) reject(err);
      else     resolve();
    });
  });
}

// ============================================================
//  MIDDLEWARE
// ============================================================
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
//  ROUTES
// ============================================================

// GET /api/grid — full 6×6 state snapshot
app.get('/api/grid', (req, res) => {
  res.json({ grid, mqtt: mqttClient.connected });
});

// POST /api/light — toggle / set a single light
// Body: { row: 0-5, light: 0-5, state: true|false }
app.post('/api/light', async (req, res) => {
  const { row, light, state } = req.body;
  if (row === undefined || light === undefined || state === undefined)
    return res.status(400).json({ error: 'row, light, state required' });

  const r = parseInt(row,   10);
  const l = parseInt(light, 10);
  const s = (state === true || state === 'true' || state === 1 || state === '1');

  if (isNaN(r) || r < 0 || r > 5 || isNaN(l) || l < 0 || l > 5)
    return res.status(400).json({ error: 'row/light out of range (0–5)' });

  try {
    await publish(TOPIC_CMD_SINGLE(r, l), s ? 'ON' : 'OFF');
    grid[r][l] = s;   // optimistic — real state confirmed when ESP32 publishes back
    console.log(`[CMD] Row ${r+1} Light ${l+1} → ${s ? 'ON' : 'OFF'}`);
    res.json({ grid, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// POST /api/row — set all 6 lights in one row
// Body: { row: 0-5, state: true|false }
app.post('/api/row', async (req, res) => {
  const { row, state } = req.body;
  if (row === undefined || state === undefined)
    return res.status(400).json({ error: 'row, state required' });

  const r = parseInt(row, 10);
  const s = (state === true || state === 'true' || state === 1 || state === '1');

  if (isNaN(r) || r < 0 || r > 5)
    return res.status(400).json({ error: 'row out of range (0–5)' });

  try {
    // One MQTT message → all 6 ESP32s in this row receive it
    await publish(TOPIC_CMD_ROW(r), s ? 'ON' : 'OFF');
    for (let l = 0; l < 6; l++) grid[r][l] = s;
    console.log(`[CMD] Row ${r+1} ALL → ${s ? 'ON' : 'OFF'}`);
    res.json({ grid, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// POST /api/all — all 36 lights at once
// Body: { state: true|false }
app.post('/api/all', async (req, res) => {
  const { state } = req.body;
  if (state === undefined)
    return res.status(400).json({ error: 'state required' });

  const s = (state === true || state === 'true' || state === 1 || state === '1');

  try {
    // One MQTT message → all 36 ESP32s receive it
    await publish(TOPIC_CMD_ALL(), s ? 'ON' : 'OFF');
    for (let r = 0; r < 6; r++)
      for (let l = 0; l < 6; l++) grid[r][l] = s;
    console.log(`[CMD] ALL LIGHTS → ${s ? 'ON' : 'OFF'}`);
    res.json({ grid, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// GET /health — server + MQTT status
app.get('/health', (req, res) => {
  res.json({
    server:    'online',
    mqtt:      mqttClient.connected ? 'connected' : 'disconnected',
    hivemq:    `${HIVEMQ_HOST}:${HIVEMQ_PORT}`,
    client_id: HIVEMQ_CLIENT,
    uptime_s:  Math.floor(process.uptime()),
    timestamp: new Date().toISOString(),
  });
});

mqttClient.on('reconnect', () => {
  console.log('[MQTT] Reconnecting...');
});

// change this existing block:
mqttClient.on('connect', () => {
  console.log('[MQTT] Connected to HiveMQ as', HIVEMQ_CLIENT);
  mqttClient.subscribe(STATE_WILDCARD, { qos: 1 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error:', err.message);
    else     console.log('[MQTT] Subscribed to', STATE_WILDCARD);
  });
});

// ============================================================
//  KEEP-ALIVE — self-ping every 5 min so Render never sleeps
// ============================================================
const RENDER_URL = process.env.RENDER_URL || 'https://high-bay-light-73hq.onrender.com';

setInterval(() => {
  const https = require('https');
  const url = `${RENDER_URL}/health`;
  https.get(url, (res) => {
    console.log('[KEEP-ALIVE] Ping OK →', url, '| Status:', res.statusCode);
  }).on('error', (e) => {
    console.warn('[KEEP-ALIVE] Ping failed:', e.message);
  });
}, 5 * 60 * 1000); // every 5 minutes

console.log('[KEEP-ALIVE] Self-ping enabled →', RENDER_URL);

// ============================================================
//  START
// ============================================================
app.listen(PORT, () => {
  console.log('\n╔════════════════════════════════════════════╗');
  console.log('║  AIPL High Bay — HiveMQ Server v2.0        ║');
  console.log('╚════════════════════════════════════════════╝');
  console.log(`\n  UI      : http://localhost:${PORT}`);
  console.log(`  Grid    : http://localhost:${PORT}/api/grid`);
  console.log(`  Health  : http://localhost:${PORT}/health`);
  console.log(`\n  HiveMQ  : ${HIVEMQ_HOST}:${HIVEMQ_PORT}`);
  console.log(`  User    : ${HIVEMQ_USERNAME}`);
  console.log(`  Client  : ${HIVEMQ_CLIENT}`);
  console.log(`  Render  : ${RENDER_URL || '(not set — local only)'}\n`);
});