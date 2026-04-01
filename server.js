'use strict';

require('dotenv').config();

const express = require('express');
const cors    = require('cors');
const path    = require('path');
const mqtt    = require('mqtt');
const app     = express();
const PORT    = process.env.PORT || 3000;

// ============================================================
//  HiveMQ Config
// ============================================================
const HIVEMQ_HOST     = process.env.HIVEMQ_HOST     || 'a0d0d0e9332a4d3db7516f6125f6e677.s1.eu.hivemq.cloud';
const HIVEMQ_PORT     = parseInt(process.env.HIVEMQ_PORT || '8883');
const HIVEMQ_USERNAME = process.env.HIVEMQ_USERNAME || 'Highbaylight';
const HIVEMQ_PASSWORD = process.env.HIVEMQ_PASSWORD || 'Naveen235623@@';
const HIVEMQ_CLIENT   = 'aipl-server-' + Math.random().toString(16).slice(2, 8);

// ============================================================
//  TOPIC HELPERS
// ============================================================
const TOPIC_CMD_SINGLE  = (r, l) => `aipl/row/${r}/light/${l}/command`;
const TOPIC_CMD_ROW     = (r)    => `aipl/row/${r}/command`;
const TOPIC_CMD_ALL     = ()     => `aipl/all/command`;
const STATE_WILDCARD    = 'aipl/row/+/light/+/state';
const TELE_WILDCARD     = 'aipl/row/+/light/+/telemetry';

// ============================================================
//  IN-MEMORY GRID  [row 0-5][light 0-5]
//  grid        = what the ESP32 is physically reporting
//  userIntent  = what the USER last commanded (this is the source of truth)
//
//  KEY FIX:
//  When ESP32 reboots after MCB power cycle, it boots ON and
//  publishes "ON" to the state topic. We detect that this
//  contradicts the userIntent and immediately push the correct
//  command back to the device.
// ============================================================
const grid       = Array.from({ length: 6 }, () => Array(6).fill(true));
const userIntent = Array.from({ length: 6 }, () => Array(6).fill(true)); // last user command

// ============================================================
//  MQTT CLIENT
// ============================================================
const mqttClient = mqtt.connect(`mqtts://${HIVEMQ_HOST}:${HIVEMQ_PORT}`, {
  username:           HIVEMQ_USERNAME,
  password:           HIVEMQ_PASSWORD,
  clientId:           HIVEMQ_CLIENT,
  rejectUnauthorized: true,
  reconnectPeriod:    3000,
  keepalive:          60,
});

mqttClient.on('connect', () => {
  console.log('[MQTT] Connected to HiveMQ as', HIVEMQ_CLIENT);

  mqttClient.subscribe(STATE_WILDCARD, { qos: 1 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error (state):', err.message);
    else     console.log('[MQTT] Subscribed to', STATE_WILDCARD);
  });

  mqttClient.subscribe(TELE_WILDCARD, { qos: 1 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error (tele):', err.message);
    else     console.log('[MQTT] Subscribed to', TELE_WILDCARD);
  });
});

// ============================================================
//  INCOMING STATE FROM ESP32
//  This is the KEY FIX:
//  When the device publishes its state (e.g. ON after reboot),
//  we compare it to userIntent. If they differ, we push the
//  correct command back immediately — so the light matches
//  whatever the user last set, even after a power cut.
// ============================================================
mqttClient.on('message', (topic, payload) => {
  const parts = topic.split('/');

  // ── State topic: aipl/row/R/light/L/state ──
  if (
    parts.length === 6   &&
    parts[0] === 'aipl'  &&
    parts[1] === 'row'   &&
    parts[3] === 'light' &&
    parts[5] === 'state'
  ) {
    const row        = parseInt(parts[2], 10);
    const light      = parseInt(parts[4], 10);
    const reportedOn = payload.toString().trim().toUpperCase() === 'ON';

    if (row >= 0 && row < 6 && light >= 0 && light < 6) {
      grid[row][light] = reportedOn;

      const intended = userIntent[row][light];

      if (reportedOn !== intended) {
        // ── RESTORE: device state does not match user intent ──
        // This happens when:
        //   • MCB was cut and device rebooted → boots ON → user had set OFF
        //   • OR device rebooted for any reason
        console.log(
          `[RESTORE] Row ${row+1} Light ${light+1} reported ${reportedOn ? 'ON' : 'OFF'} ` +
          `but user intent is ${intended ? 'ON' : 'OFF'} → pushing correction`
        );

        // Small delay so device has finished its boot sequence
        setTimeout(() => {
          publish(TOPIC_CMD_SINGLE(row, light), intended ? 'ON' : 'OFF')
            .then(() => {
              console.log(`[RESTORE] Correction sent → Row ${row+1} Light ${light+1} = ${intended ? 'ON' : 'OFF'}`);
            })
            .catch(e => {
              console.error(`[RESTORE] Failed to send correction:`, e.message);
            });
        }, 2000); // 2 second delay — gives ESP32 time to finish connecting

      } else {
        console.log(`[STATE] Row ${row+1} Light ${light+1} → ${reportedOn ? 'ON' : 'OFF'} ✓ matches intent`);
      }
    }
    return;
  }

  // ── Telemetry topic: aipl/row/R/light/L/telemetry ──
  if (
    parts.length === 6   &&
    parts[0] === 'aipl'  &&
    parts[1] === 'row'   &&
    parts[3] === 'light' &&
    parts[5] === 'telemetry'
  ) {
    // Just log telemetry — extend here if you want to store metrics
    const row   = parseInt(parts[2], 10);
    const light = parseInt(parts[4], 10);
    try {
      const data = JSON.parse(payload.toString());
      console.log(`[TELE] Row ${row+1} Light ${light+1} | uptime:${data.uptime_s}s rssi:${data.rssi}dBm kwh:${data.kwh_used}`);
    } catch { /* ignore bad JSON */ }
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
  res.json({ grid, userIntent, mqtt: mqttClient.connected });
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

    // Save what the USER intended — this is used to restore after power cycle
    userIntent[r][l] = s;
    grid[r][l]       = s;  // optimistic

    console.log(`[CMD] Row ${r+1} Light ${l+1} → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
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
    await publish(TOPIC_CMD_ROW(r), s ? 'ON' : 'OFF');

    // Save intent for all 6 lights in this row
    for (let l = 0; l < 6; l++) {
      userIntent[r][l] = s;
      grid[r][l]       = s;
    }

    console.log(`[CMD] Row ${r+1} ALL → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
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
    await publish(TOPIC_CMD_ALL(), s ? 'ON' : 'OFF');

    // Save intent for all 36 lights
    for (let r = 0; r < 6; r++)
      for (let l = 0; l < 6; l++) {
        userIntent[r][l] = s;
        grid[r][l]       = s;
      }

    console.log(`[CMD] ALL LIGHTS → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
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

// ============================================================
//  KEEP-ALIVE — self-ping every 5 min so Render never sleeps
// ============================================================
const RENDER_URL = process.env.RENDER_URL || 'https://high-bay-light-73hq.onrender.com';

setInterval(() => {
  const https = require('https');
  https.get(`${RENDER_URL}/health`, (res) => {
    console.log('[KEEP-ALIVE] Ping OK | Status:', res.statusCode);
  }).on('error', (e) => {
    console.warn('[KEEP-ALIVE] Ping failed:', e.message);
  });
}, 5 * 60 * 1000);

console.log('[KEEP-ALIVE] Self-ping enabled →', RENDER_URL);

// ============================================================
//  START
// ============================================================
app.listen(PORT, () => {
  console.log('\n╔════════════════════════════════════════════╗');
  console.log('║  AIPL High Bay — HiveMQ Server v2.1        ║');
  console.log('║  FIX: MCB power cycle restores user state  ║');
  console.log('╚════════════════════════════════════════════╝');
  console.log(`\n  UI      : http://localhost:${PORT}`);
  console.log(`  Grid    : http://localhost:${PORT}/api/grid`);
  console.log(`  Health  : http://localhost:${PORT}/health`);
  console.log(`\n  HiveMQ  : ${HIVEMQ_HOST}:${HIVEMQ_PORT}`);
  console.log(`  User    : ${HIVEMQ_USERNAME}`);
  console.log(`  Client  : ${HIVEMQ_CLIENT}`);
  console.log(`  Render  : ${RENDER_URL || '(not set — local only)'}\n`);
});