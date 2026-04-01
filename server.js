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
const TOPIC_CMD_SINGLE = (r, l) => `aipl/row/${r}/light/${l}/command`;
const TOPIC_CMD_ROW    = (r)    => `aipl/row/${r}/command`;
const TOPIC_CMD_ALL    = ()     => `aipl/all/command`;
const STATE_WILDCARD   = 'aipl/row/+/light/+/state';
const TELE_WILDCARD    = 'aipl/row/+/light/+/telemetry';

// ============================================================
//  IN-MEMORY GRIDS
//
//  grid             = what the ESP32 physically reported
//  userIntent       = what the USER last commanded (source of truth)
//  correctionSent   = flag: did we already send a correction after last reboot?
//  lastCorrection   = timestamp of when we sent the correction
//
//  WHY THE LOOP HAPPENED IN v2.1:
//    1. ESP32 reboots → publishes "ON"
//    2. Server sees mismatch → sends "OFF" after 2s
//    3. ESP32 turns OFF → publishes "OFF"   ← triggers message handler again
//    4. Server sees "OFF" vs userIntent(false) → thinks it's a NEW mismatch → sends "ON"
//    5. Loop forever
//
//  THE FIX (v2.2):
//    After sending a correction, set correctionSent[r][l] = true
//    and block ALL further auto-corrections for 10 seconds (cooldown).
//    The correction flag is only cleared when the USER manually
//    sends a new command — never automatically.
// ============================================================
const grid             = Array.from({ length: 6 }, () => Array(6).fill(true));
const userIntent       = Array.from({ length: 6 }, () => Array(6).fill(true));
const correctionSent   = Array.from({ length: 6 }, () => Array(6).fill(false));
const lastCorrection   = Array.from({ length: 6 }, () => Array(6).fill(0));

const COOLDOWN_MS = 10000; // 10s cooldown after sending a correction

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

    if (row < 0 || row > 5 || light < 0 || light > 5) return;

    // Always update the display grid
    grid[row][light] = reportedOn;

    const intended = userIntent[row][light];
    const now      = Date.now();

    // ── COOLDOWN CHECK ──
    // If we already sent a correction recently, do NOT send another one.
    // This is what stops the ON → OFF → ON → OFF loop.
    if (correctionSent[row][light]) {
      const elapsed = now - lastCorrection[row][light];
      if (elapsed < COOLDOWN_MS) {
        console.log(
          `[COOLDOWN] Row ${row+1} Light ${light+1} → ignoring state report ` +
          `(${Math.round(elapsed / 1000)}s into ${COOLDOWN_MS / 1000}s cooldown)`
        );
        return; // ← do nothing, wait for cooldown to expire
      } else {
        // Cooldown expired — allow checking again
        correctionSent[row][light] = false;
      }
    }

    // ── MISMATCH CHECK ──
    if (reportedOn !== intended) {
      console.log(
        `[RESTORE] Row ${row+1} Light ${light+1} reported ${reportedOn ? 'ON' : 'OFF'} ` +
        `but user wants ${intended ? 'ON' : 'OFF'} → correcting in 2s`
      );

      // Lock correction flag IMMEDIATELY so any messages during the 2s
      // wait don't trigger a second correction
      correctionSent[row][light] = true;
      lastCorrection[row][light] = now;

      setTimeout(() => {
        publish(TOPIC_CMD_SINGLE(row, light), intended ? 'ON' : 'OFF')
          .then(() => {
            grid[row][light] = intended; // optimistic update
            console.log(`[RESTORE] ✓ Corrected → Row ${row+1} Light ${light+1} = ${intended ? 'ON' : 'OFF'}`);
          })
          .catch(e => {
            console.error(`[RESTORE] Publish failed:`, e.message);
            correctionSent[row][light] = false; // retry allowed on next state report
          });
      }, 2000);

    } else {
      // State already matches intent — no correction needed
      correctionSent[row][light] = false;
      console.log(`[STATE] Row ${row+1} Light ${light+1} → ${reportedOn ? 'ON' : 'OFF'} ✓ matches intent`);
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
    const row   = parseInt(parts[2], 10);
    const light = parseInt(parts[4], 10);
    try {
      const data = JSON.parse(payload.toString());
      console.log(
        `[TELE] Row ${row+1} Light ${light+1} | uptime:${data.uptime_s}s rssi:${data.rssi}dBm kwh:${data.kwh_used}`
      );
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

// GET /api/grid
app.get('/api/grid', (req, res) => {
  res.json({ grid, userIntent, mqtt: mqttClient.connected });
});

// POST /api/light — single light
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

    userIntent[r][l]     = s;     // save user intent
    correctionSent[r][l] = false; // reset so next reboot gets fresh correction
    grid[r][l]           = s;

    console.log(`[CMD] Row ${r+1} Light ${l+1} → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// POST /api/row — entire row
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

    for (let l = 0; l < 6; l++) {
      userIntent[r][l]     = s;
      correctionSent[r][l] = false;
      grid[r][l]           = s;
    }

    console.log(`[CMD] Row ${r+1} ALL → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// POST /api/all — all 36 lights
app.post('/api/all', async (req, res) => {
  const { state } = req.body;
  if (state === undefined)
    return res.status(400).json({ error: 'state required' });

  const s = (state === true || state === 'true' || state === 1 || state === '1');

  try {
    await publish(TOPIC_CMD_ALL(), s ? 'ON' : 'OFF');

    for (let r = 0; r < 6; r++)
      for (let l = 0; l < 6; l++) {
        userIntent[r][l]     = s;
        correctionSent[r][l] = false;
        grid[r][l]           = s;
      }

    console.log(`[CMD] ALL LIGHTS → ${s ? 'ON' : 'OFF'} (intent saved)`);
    res.json({ grid, userIntent, mqtt: mqttClient.connected });
  } catch (e) {
    console.error('[CMD] publish error:', e.message);
    res.status(503).json({ error: 'MQTT publish failed', detail: e.message });
  }
});

// GET /health
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

// ============================================================
//  START
// ============================================================
app.listen(PORT, () => {
  console.log('\n╔════════════════════════════════════════════╗');
  console.log('║  AIPL High Bay — HiveMQ Server v2.2        ║');
  console.log('║  FIX: No more ON/OFF loop after MCB cycle  ║');
  console.log('╚════════════════════════════════════════════╝');
  console.log(`\n  UI     : http://localhost:${PORT}`);
  console.log(`  Grid   : http://localhost:${PORT}/api/grid`);
  console.log(`  Health : http://localhost:${PORT}/health\n`);
});