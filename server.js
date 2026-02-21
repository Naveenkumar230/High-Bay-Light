'use strict';

require('dotenv').config();

const express = require('express');
const axios   = require('axios');
const cors    = require('cors');
const path    = require('path');

const app  = express();
const PORT = process.env.PORT || 3000;

// ── ThingsBoard Config ──
const TB_HOST      = process.env.TB_HOST      || 'https://thingsboard.cloud';
const TB_TOKEN     = process.env.TB_TOKEN     || 'J1R7Lw0dNx17T6HVifjX'; // device token
const TB_EMAIL     = process.env.TB_EMAIL     || '';  // your ThingsBoard login email
const TB_PASSWORD  = process.env.TB_PASSWORD  || '';  // your ThingsBoard login password
const TB_DEVICE_ID = process.env.TB_DEVICE_ID || '';  // device ID from ThingsBoard

// ── In-memory ──
let jwtToken    = null;
let jwtExpiry   = 0;
let cachedState = null;

// ============================================================
//  MIDDLEWARE
// ============================================================
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
//  JWT LOGIN — get ThingsBoard user token
// ============================================================
async function getJWT() {
  if (jwtToken && Date.now() < jwtExpiry) return jwtToken;
  const r = await axios.post(`${TB_HOST}/api/auth/login`, {
    username: TB_EMAIL,
    password: TB_PASSWORD
  }, { timeout: 8000 });
  jwtToken  = r.data.token;
  jwtExpiry = Date.now() + 50 * 60 * 1000; // refresh every 50 min
  console.log('[JWT] Token refreshed');
  return jwtToken;
}

// ============================================================
//  THINGSBOARD HELPERS
// ============================================================

// Read latest telemetry via JWT
async function tbGetStatus() {
  const jwt = await getJWT();
  const url = `${TB_HOST}/api/plugins/telemetry/DEVICE/${TB_DEVICE_ID}/values/timeseries?keys=light_state,rssi,kwh_used,on_seconds,off_seconds,firmware`;
  const r   = await axios.get(url, {
    headers: { 'X-Authorization': `Bearer ${jwt}` },
    timeout: 8000
  });
  const d   = r.data;
  const get = (key) => d[key]?.[0]?.value;
  return {
    state:       get('light_state') === true || get('light_state') === 'true',
    rssi:        get('rssi')        ?? '--',
    kwh:         get('kwh_used')    ?? 0,
    on_seconds:  get('on_seconds')  ?? 0,
    off_seconds: get('off_seconds') ?? 0,
    firmware:    get('firmware')    ?? '--',
    mqtt:        true
  };
}

// Send RPC to ESP32 via ThingsBoard device token
async function tbSetLight(desired) {
  const url  = `${TB_HOST}/api/v1/${TB_TOKEN}/rpc`;
  const body = { method: 'setLight', params: { state: desired } };
  const r    = await axios.post(url, body, { timeout: 10000 });
  return r.data;
}

// ============================================================
//  ROUTES
// ============================================================

// GET /proxy/status
app.get('/proxy/status', async (req, res) => {
  try {
    const status = await tbGetStatus();
    cachedState  = status.state;
    res.json(status);
  } catch (e) {
    console.error('[STATUS] Error:', e.message);
    res.status(503).json({ error: 'ThingsBoard unreachable', detail: e.message });
  }
});

// POST /proxy/set?state=1 or 0
app.post('/proxy/set', async (req, res) => {
  const sv = req.query.state ?? req.body.state;
  if (sv === undefined) return res.status(400).json({ error: 'state param required' });
  const desired = sv === '1' || sv === true || sv === 'true';
  try {
    await tbSetLight(desired);
    await new Promise(r => setTimeout(r, 1000));
    const status = await tbGetStatus();
    cachedState  = status.state;
    res.json(status);
  } catch (e) {
    console.error('[SET] Error:', e.message);
    res.status(503).json({ error: 'Command failed', detail: e.message });
  }
});

// GET /health
app.get('/health', (req, res) => {
  res.json({
    server:    'online',
    tb_host:   TB_HOST,
    uptime_s:  Math.floor(process.uptime()),
    lastState: cachedState,
    timestamp: new Date().toISOString()
  });
});

// ============================================================
//  START
// ============================================================
app.listen(PORT, () => {
  console.log('\n╔══════════════════════════════════════╗');
  console.log('║  AIPL Light Controller — server.js   ║');
  console.log('╚══════════════════════════════════════╝');
  console.log(`\n  UI      : http://localhost:${PORT}`);
  console.log(`  Status  : http://localhost:${PORT}/proxy/status`);
  console.log(`  Set ON  : POST http://localhost:${PORT}/proxy/set?state=1`);
  console.log(`  Health  : http://localhost:${PORT}/health`);
  console.log(`\n  TB Host : ${TB_HOST}`);
  console.log(`  Device  : ${TB_DEVICE_ID || '⚠ NOT SET — add TB_DEVICE_ID to .env'}`);
  console.log(`  Email   : ${TB_EMAIL     || '⚠ NOT SET — add TB_EMAIL to .env'}\n`);
});