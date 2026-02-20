/* ============================================================
   AIPL High Bay Controller — server.js
   Node.js Backend | v6.0
   
   Features:
   - Serves frontend (index.html, style.css, app.js)
   - Proxies API calls to ESP32 (fixes CORS)
   - Logs every ON/OFF event to logs/events.json
   - Email alerts on state change
   - ThingsBoard HTTP integration endpoint
   - Daily summary API
   - Health check endpoint

   Install: npm install express axios nodemailer cors dotenv
   Run:     node server.js
   Config:  copy .env.example → .env and fill values
   ============================================================ */

'use strict';

require('dotenv').config();

const express    = require('express');
const axios      = require('axios');
const nodemailer = require('nodemailer');
const cors       = require('cors');
const fs         = require('fs');
const path       = require('path');

const app  = express();
const PORT = process.env.PORT || 3000;

// ── Config ──
const ESP32_IP         = process.env.ESP32_IP         || '192.168.1.100';
const ALERTS_ENABLED   = process.env.ALERTS_ENABLED   === 'true';
const ALERT_EMAIL_TO   = process.env.ALERT_EMAIL_TO   || '';
const ALERT_EMAIL_FROM = process.env.ALERT_EMAIL_FROM || '';
const SMTP_HOST        = process.env.SMTP_HOST        || 'smtp.gmail.com';
const SMTP_PORT        = parseInt(process.env.SMTP_PORT || '587');
const SMTP_USER        = process.env.SMTP_USER        || '';
const SMTP_PASS        = process.env.SMTP_PASS        || '';
const LOG_FILE         = path.join(__dirname, 'logs', 'events.json');

// ── Ensure logs/ folder exists ──
const logsDir = path.join(__dirname, 'logs');
if (!fs.existsSync(logsDir)) fs.mkdirSync(logsDir);

// ── In-memory ──
let lastKnownState = null;

// ============================================================
//  MIDDLEWARE
// ============================================================
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

app.use(express.static(path.join(__dirname, 'public')));

// Serve frontend files from root of project
app.use(express.static(path.join(__dirname, '..')));

// Also serve app.js from src/
app.use('/src', express.static(path.join(__dirname, '..', 'src')));

// ============================================================
//  LOGGING
// ============================================================
function logEvent(event) {
  let log = [];
  try { log = JSON.parse(fs.readFileSync(LOG_FILE, 'utf8')); } catch {}
  log.push({ timestamp: new Date().toISOString(), ...event });
  if (log.length > 10000) log = log.slice(-10000);
  fs.writeFileSync(LOG_FILE, JSON.stringify(log, null, 2));
  console.log(`[LOG] ${event.type} | state=${event.state} | src=${event.source}`);
}

// ============================================================
//  EMAIL ALERTS
// ============================================================
const mailer = nodemailer.createTransport({
  host: SMTP_HOST, port: SMTP_PORT,
  secure: SMTP_PORT === 465,
  auth: { user: SMTP_USER, pass: SMTP_PASS }
});

async function sendAlert(subject, body) {
  if (!ALERTS_ENABLED || !ALERT_EMAIL_TO) return;
  try {
    await mailer.sendMail({
      from: ALERT_EMAIL_FROM || SMTP_USER,
      to:   ALERT_EMAIL_TO,
      subject,
      text: body,
      html: `<div style="font-family:sans-serif;padding:20px;max-width:500px">
        <h2 style="color:#1a6bff;margin-bottom:12px">&#9889; AIPL Light Alert</h2>
        <p style="font-size:15px;line-height:1.6">${body}</p>
        <hr style="border:none;border-top:1px solid #e2e8f8;margin:16px 0"/>
        <p style="color:#9ca3af;font-size:11px">AIPL High Bay Controller v6.0</p>
      </div>`
    });
    console.log('[ALERT] Email sent →', ALERT_EMAIL_TO);
  } catch (e) {
    console.error('[ALERT] Email failed:', e.message);
  }
}

// ============================================================
//  STATE CHANGE HANDLER
// ============================================================
async function onStateChange(newState, source) {
  if (lastKnownState === newState) return;
  lastKnownState = newState;
  logEvent({ type: 'STATE_CHANGE', state: newState, source });
  if (ALERTS_ENABLED) {
    const label = newState ? 'TURNED ON' : 'TURNED OFF';
    const time  = new Date().toLocaleString();
    await sendAlert(
      `AIPL High Bay Light ${label} — ${time}`,
      `The High Bay light has been <strong>${label}</strong>.<br/>
       Time: ${time}<br/>Source: ${source}<br/>Device IP: ${ESP32_IP}`
    );
  }
}

// ============================================================
//  PROXY ROUTES → ESP32
// ============================================================

// GET /proxy/status
app.get('/proxy/status', async (req, res) => {
  try {
    const r = await axios.get(`http://${ESP32_IP}/api/status`, { timeout: 5000 });
    await onStateChange(r.data.state, 'server-poll');
    res.json(r.data);
  } catch (e) {
    res.status(503).json({ error: 'ESP32 unreachable', detail: e.message });
  }
});

// POST /proxy/set?state=1 or state=0
app.post('/proxy/set', async (req, res) => {
  const sv = req.query.state ?? req.body.state;
  if (sv === undefined) return res.status(400).json({ error: 'state param required' });
  const desired = sv === '1' || sv === true || sv === 'true';
  try {
    const r = await axios.post(
      `http://${ESP32_IP}/api/set?state=${desired ? 1 : 0}`, {}, { timeout: 5000 }
    );
    await onStateChange(r.data.state, 'api-proxy');
    res.json(r.data);
  } catch (e) {
    res.status(503).json({ error: 'ESP32 unreachable', detail: e.message });
  }
});

// ============================================================
//  THINGSBOARD HTTP INTEGRATION
// ============================================================

// POST /tb/rpc  — ThingsBoard sends RPC commands here
app.post('/tb/rpc', async (req, res) => {
  const { method, params } = req.body;
  console.log('[TB-RPC]', method, params);
  if (method === 'setLight') {
    const desired = params?.state === true || params?.state === 'true';
    try {
      const r = await axios.post(
        `http://${ESP32_IP}/api/set?state=${desired ? 1 : 0}`, {}, { timeout: 5000 }
      );
      await onStateChange(r.data.state, 'thingsboard-rpc');
      res.json({ ok: true, state: r.data.state });
    } catch (e) {
      res.status(503).json({ error: 'ESP32 unreachable' });
    }
  } else {
    res.status(400).json({ error: 'Unknown method: ' + method });
  }
});

// GET /tb/telemetry — ThingsBoard HTTP data source pull
app.get('/tb/telemetry', async (req, res) => {
  try {
    const r = await axios.get(`http://${ESP32_IP}/api/status`, { timeout: 5000 });
    res.json(r.data);
  } catch (e) {
    res.status(503).json({ error: 'ESP32 unreachable' });
  }
});

// ============================================================
//  LOGS API
// ============================================================

// GET /logs  (optional: ?limit=100&date=2025-02-20)
app.get('/logs', (req, res) => {
  let log = [];
  try { log = JSON.parse(fs.readFileSync(LOG_FILE, 'utf8')); } catch {}
  if (req.query.date) log = log.filter(e => e.timestamp.startsWith(req.query.date));
  const limit = parseInt(req.query.limit || '500');
  res.json(log.slice(-limit));
});

// GET /logs/summary
app.get('/logs/summary', (req, res) => {
  let log = [];
  try { log = JSON.parse(fs.readFileSync(LOG_FILE, 'utf8')); } catch {}
  const today  = new Date().toISOString().split('T')[0];
  const events = log.filter(e => e.timestamp.startsWith(today) && e.type === 'STATE_CHANGE');
  res.json({
    date:       today,
    turnedOn:   events.filter(e => e.state  === true).length,
    turnedOff:  events.filter(e => e.state  === false).length,
    totalEvents: events.length,
    events
  });
});

// DELETE /logs
app.delete('/logs', (req, res) => {
  fs.writeFileSync(LOG_FILE, '[]');
  res.json({ ok: true, message: 'Logs cleared' });
});

// ============================================================
//  HEALTH CHECK
// ============================================================
app.get('/health', async (req, res) => {
  let esp32Online = false;
  try {
    await axios.get(`http://${ESP32_IP}/api/status`, { timeout: 3000 });
    esp32Online = true;
  } catch {}
  res.json({
    server:    'online',
    esp32:     esp32Online ? 'online' : 'offline',
    esp32_ip:  ESP32_IP,
    uptime_s:  Math.floor(process.uptime()),
    alerts:    ALERTS_ENABLED,
    timestamp: new Date().toISOString()
  });
});

// ============================================================
//  SERVER-SIDE PERIODIC POLL (for logging without browser open)
// ============================================================
setInterval(async () => {
  try {
    const r = await axios.get(`http://${ESP32_IP}/api/status`, { timeout: 4000 });
    await onStateChange(r.data.state, 'server-bg-poll');
  } catch {}
}, 10000);

// ============================================================
//  START
// ============================================================
app.listen(PORT, () => {
  console.log('\n╔══════════════════════════════════════╗');
  console.log('║  AIPL Light Controller — server.js   ║');
  console.log('╚══════════════════════════════════════╝');
  console.log(`\n  UI       : http://localhost:${PORT}`);
  console.log(`  Status   : http://localhost:${PORT}/proxy/status`);
  console.log(`  Set ON   : POST http://localhost:${PORT}/proxy/set?state=1`);
  console.log(`  TB-RPC   : POST http://localhost:${PORT}/tb/rpc`);
  console.log(`  Logs     : http://localhost:${PORT}/logs`);
  console.log(`  Summary  : http://localhost:${PORT}/logs/summary`);
  console.log(`  Health   : http://localhost:${PORT}/health`);
  console.log(`\n  ESP32    : ${ESP32_IP}`);
  console.log(`  Alerts   : ${ALERTS_ENABLED ? 'ON → ' + ALERT_EMAIL_TO : 'off'}\n`);
});