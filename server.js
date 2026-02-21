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
const TB_EMAIL     = process.env.TB_EMAIL     || 'naveenkumarak2002@gmail.com';
const TB_PASSWORD  = process.env.TB_PASSWORD  || 'Naveen235623@@@';
const TB_DEVICE_ID = process.env.TB_DEVICE_ID || '801f4020-0e45-11f1-b6d0-133d513d174e';

// ── Render keep-alive URL (set this to your Render public URL) ──
const RENDER_URL   = process.env.RENDER_URL   || '';

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
//  JWT LOGIN
// ============================================================
async function getJWT() {
  if (jwtToken && Date.now() < jwtExpiry) return jwtToken;
  const r = await axios.post(`${TB_HOST}/api/auth/login`, {
    username: TB_EMAIL,
    password: TB_PASSWORD
  }, { timeout: 8000 });
  jwtToken  = r.data.token;
  jwtExpiry = Date.now() + 50 * 60 * 1000;
  console.log('[JWT] Token refreshed');
  return jwtToken;
}

// ============================================================
//  THINGSBOARD HELPERS
// ============================================================

// Read latest telemetry
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

// Send server-side RPC to device via JWT
async function tbSetLight(desired) {
  const jwt  = await getJWT();
  const url  = `${TB_HOST}/api/rpc/oneway/${TB_DEVICE_ID}`;
  const body = {
    method:     'setLight',
    params:     { state: desired },
    timeout:    5000,
    persistent: false
  };
  const r = await axios.post(url, body, {
    headers: { 'X-Authorization': `Bearer ${jwt}` },
    timeout: 10000
  });
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
    console.log(`[SET] Light → ${desired ? 'ON' : 'OFF'}`);
    await new Promise(r => setTimeout(r, 1500));
    const status = await tbGetStatus();
    cachedState  = status.state;
    res.json(status);
  } catch (e) {
    console.error('[SET] Error:', e.message);
    res.status(503).json({ error: 'Command failed', detail: e.message });
  }
});

// GET /health — also used as keep-alive ping target
app.get('/health', (req, res) => {
  res.json({
    server:    'online',
    tb_host:   TB_HOST,
    device_id: TB_DEVICE_ID,
    uptime_s:  Math.floor(process.uptime()),
    lastState: cachedState,
    timestamp: new Date().toISOString()
  });
});

// ============================================================
//  KEEP-ALIVE — self-ping every 10 minutes so Render never sleeps
// ============================================================
function keepAlive() {
  if (!RENDER_URL) return; // only runs when RENDER_URL is set
  setInterval(async () => {
    try {
      await axios.get(`${RENDER_URL}/health`, { timeout: 8000 });
      console.log('[KEEP-ALIVE] Ping sent →', RENDER_URL);
    } catch (e) {
      console.warn('[KEEP-ALIVE] Ping failed:', e.message);
    }
  }, 10 * 60 * 1000); // every 10 minutes
}

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
  console.log(`  Device  : ${TB_DEVICE_ID || '⚠ NOT SET'}`);
  console.log(`  Email   : ${TB_EMAIL     || '⚠ NOT SET'}`);
  console.log(`  Render  : ${RENDER_URL   || '⚠ NOT SET — add RENDER_URL to env'}\n`);
  keepAlive(); // start self-ping
});