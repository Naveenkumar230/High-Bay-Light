'use strict';

require('dotenv').config();

const express = require('express');
const axios   = require('axios');
const cors    = require('cors');
const path    = require('path');

const app  = express();
const PORT = process.env.PORT || 3000;

// ── ThingsBoard Config ──
const TB_HOST  = process.env.TB_HOST  || 'https://thingsboard.cloud';
const TB_TOKEN = process.env.TB_TOKEN || 'J1R7Lw0dNx17T6HVifjX';

// ── In-memory state cache ──
let cachedState = null;

// ============================================================
//  MIDDLEWARE
// ============================================================
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// Serve frontend from public/
app.use(express.static(path.join(__dirname, 'public')));

// ============================================================
//  HELPERS — ThingsBoard REST API
// ============================================================

// Send RPC command to ESP32 via ThingsBoard
async function tbSetLight(desired) {
  const url = `${TB_HOST}/api/v1/${TB_TOKEN}/rpc`;
  const body = {
    method: 'setLight',
    params: { state: desired },
    timeout: 5000
  };
  const r = await axios.post(url, body, { timeout: 8000 });
  return r.data; // ThingsBoard returns the ESP32 RPC response
}

// Get latest telemetry from ThingsBoard
async function tbGetStatus() {
  const url = `${TB_HOST}/api/v1/${TB_TOKEN}/telemetry?keys=light_state,rssi,kwh_used,on_seconds,off_seconds,firmware`;
  const r = await axios.get(url, { timeout: 8000 });
  const d = r.data;

  // ThingsBoard returns { key: [{ts, value}] } — extract latest values
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

// ============================================================
//  ROUTES
// ============================================================

// GET /proxy/status — returns light state from ThingsBoard telemetry
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

// POST /proxy/set?state=1 or 0 — sends RPC to ESP32 via ThingsBoard
app.post('/proxy/set', async (req, res) => {
  const sv = req.query.state ?? req.body.state;
  if (sv === undefined) return res.status(400).json({ error: 'state param required' });

  const desired = sv === '1' || sv === true || sv === 'true';

  try {
    const rpcResp = await tbSetLight(desired);
    cachedState   = rpcResp?.state ?? desired;

    // Small delay then fetch fresh telemetry to confirm
    await new Promise(r => setTimeout(r, 800));
    const status = await tbGetStatus();
    res.json(status);
  } catch (e) {
    console.error('[SET] Error:', e.message);
    res.status(503).json({ error: 'Command failed', detail: e.message });
  }
});

// GET /health
app.get('/health', (req, res) => {
  res.json({
    server:     'online',
    tb_host:    TB_HOST,
    uptime_s:   Math.floor(process.uptime()),
    lastState:  cachedState,
    timestamp:  new Date().toISOString()
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
  console.log(`  Token   : ${TB_TOKEN.slice(0, 6)}...****\n`);
});