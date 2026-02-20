/* ============================================================
   AIPL High Bay Controller — app.js
   Frontend JavaScript — all logic separated from HTML/CSS
   ============================================================ */

'use strict';

// ── Config ──
let ESP32_IP      = localStorage.getItem('esp32_ip') || '';
const POLL_MS     = 5000;
const TICK_MS     = 1000;
const WATT        = 150;

// ── State ──
let isOn     = false;
let onSec    = 0;
let offSec   = 0;
let busy     = false;
let lastTick = Date.now();

// ============================================================
//  DOM REFERENCES
// ============================================================
const elLens       = document.getElementById('lens');
const elBeam       = document.getElementById('beam');
const elFloor      = document.getElementById('floorGlow');
const elStateLabel = document.getElementById('stateLabel');
const elBtnOn      = document.getElementById('btnOn');
const elBtnOff     = document.getElementById('btnOff');
const elConnPill   = document.getElementById('connPill');
const elConnLabel  = document.getElementById('connLabel');
const elLegOn      = document.getElementById('legOn');
const elLegOff     = document.getElementById('legOff');
const elTimeOn     = document.getElementById('timeOnBadge');
const elKwh        = document.getElementById('kwhVal');
const elRssi       = document.getElementById('rssiVal');
const elCfgIP      = document.getElementById('cfgIP');
const elCfgStatus  = document.getElementById('cfgStatus');
const elCfgSave    = document.getElementById('cfgSaveBtn');
const elCfgReset   = document.getElementById('cfgResetBtn');
const donutCanvas  = document.getElementById('donut');
const ctx          = donutCanvas.getContext('2d');

// ============================================================
//  VISUAL STATE
// ============================================================
function applyVisual(on) {
  isOn = on;
  elLens.className       = 'hb-lens '     + (on ? 'on'  : 'off');
  elBeam.className       = 'beam '        + (on ? 'on'  : '');
  elFloor.className      = 'floor-glow '  + (on ? 'on'  : '');
  elStateLabel.textContent = on ? 'ON' : 'OFF';
  elStateLabel.className   = 'light-state-lbl ' + (on ? 'on' : 'off');
  elBtnOn.classList.toggle('active-state',  on);
  elBtnOff.classList.toggle('active-state', !on);
}

function setConnectionStatus(online) {
  elConnPill.className    = 'conn-pill ' + (online ? 'connected' : 'disconnected');
  elConnLabel.textContent = online ? 'ONLINE' : 'OFFLINE';
}

// ============================================================
//  TIME FORMAT HELPERS
// ============================================================
function fmtLong(s) {
  const h  = Math.floor(s / 3600);
  const m  = Math.floor((s % 3600) / 60);
  const sc = s % 60;
  return `${h}h ${String(m).padStart(2,'0')}m ${String(sc).padStart(2,'0')}s`;
}
function fmtShort(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${h}h ${String(m).padStart(2,'0')}m`;
}

// ============================================================
//  LIGHT CONTROL — POST exact desired state to ESP32
// ============================================================
function setLight(desired) {
  if (busy) return;
  if (isOn === desired) return;
  if (!ESP32_IP) { alert('Enter ESP32 IP in Connection Settings below'); return; }

  busy = true;
  elBtnOn.disabled  = true;
  elBtnOff.disabled = true;

  fetch(`http://${ESP32_IP}/api/set?state=` + (desired ? '1' : '0'), {
    method: 'POST',
    signal: AbortSignal.timeout(5000)
  })
  .then(r => r.json())
  .then(d => {
    if (d.state !== undefined) {
      applyVisual(d.state);
      if (d.on_seconds  !== undefined) onSec  = d.on_seconds;
      if (d.off_seconds !== undefined) offSec = d.off_seconds;
      lastTick = Date.now();
      setConnectionStatus(true);
    }
  })
  .catch(() => setConnectionStatus(false))
  .finally(() => {
    busy = false;
    elBtnOn.disabled  = false;
    elBtnOff.disabled = false;
  });
}

// ── Button listeners ──
elBtnOn.addEventListener('click',  () => setLight(true));
elBtnOff.addEventListener('click', () => setLight(false));

// ============================================================
//  STATUS POLLING — sync from ESP32 every 5 seconds
// ============================================================
function pollStatus() {
  if (!ESP32_IP) return;
  fetch(`http://${ESP32_IP}/api/status`, { signal: AbortSignal.timeout(4000) })
    .then(r => r.json())
    .then(d => {
      if (d.state       !== undefined) applyVisual(d.state);
      if (d.on_seconds  !== undefined) onSec  = d.on_seconds;
      if (d.off_seconds !== undefined) offSec = d.off_seconds;
      if (d.rssi        !== undefined) elRssi.textContent = d.rssi + ' dBm';
      lastTick = Date.now();
      setConnectionStatus(true);
    })
    .catch(() => setConnectionStatus(false));
}

// ============================================================
//  LOCAL SECOND TICKER — smooth UI between polls
// ============================================================
function tick() {
  const elapsed = Math.floor((Date.now() - lastTick) / 1000);
  const liveOn  = isOn ? onSec  + elapsed : onSec;
  const liveOff = isOn ? offSec : offSec + elapsed;

  elLegOn.textContent  = fmtLong(liveOn);
  elLegOff.textContent = fmtLong(liveOff);
  elTimeOn.textContent = fmtShort(liveOn);
  elKwh.textContent    = ((WATT / 1000) * (liveOn / 3600)).toFixed(3);

  drawDonut(liveOn, liveOff);
}

// ============================================================
//  DONUT CHART
// ============================================================
function drawDonut(on, off) {
  const total = (on + off) || 1;
  const W = 180, cx = 90, cy = 90, R = 76, r = 50;
  ctx.clearRect(0, 0, W, W);

  // Draw slices
  const slices = [
    { val: on,  color: '#1a6bff' },
    { val: off, color: '#dce8ff' }
  ];
  let start = -Math.PI / 2;
  slices.forEach(sl => {
    const sweep = (sl.val / total) * 2 * Math.PI;
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, R, start, start + sweep);
    ctx.closePath();
    ctx.fillStyle = sl.color;
    ctx.fill();
    start += sweep;
  });

  // Donut hole
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, 2 * Math.PI);
  ctx.fillStyle = '#ffffff';
  ctx.fill();

  // Centre text
  const pct = Math.round((on / total) * 100);
  ctx.fillStyle    = '#1a6bff';
  ctx.font         = 'bold 22px "Space Mono", monospace';
  ctx.textAlign    = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(pct + '%', cx, cy - 8);
  ctx.fillStyle = '#6b7280';
  ctx.font      = '11px "DM Sans", sans-serif';
  ctx.fillText('ON TIME', cx, cy + 10);
}
drawDonut(0, 1);

// ============================================================
//  CONNECTION CONFIG
// ============================================================
function saveConfig() {
  const ip = elCfgIP.value.trim();
  if (!ip) { alert('Enter ESP32 IP address'); return; }
  ESP32_IP = ip;
  localStorage.setItem('esp32_ip', ip);
  elCfgStatus.textContent = '✓ Saved. Testing connection...';
  pollStatus();
}

function resetDevice() {
  if (!ESP32_IP) { alert('Set ESP32 IP first'); return; }
  if (!confirm('Clear WiFi config on ESP32 and restart?')) return;
  fetch(`http://${ESP32_IP}/reset`)
    .then(() => {
      elCfgStatus.textContent = '✓ Reset sent. Device restarting...';
      setConnectionStatus(false);
    })
    .catch(() => {
      elCfgStatus.textContent = '✗ Could not reach device.';
    });
}

elCfgSave.addEventListener('click',  saveConfig);
elCfgReset.addEventListener('click', resetDevice);

// Load saved IP
if (ESP32_IP) elCfgIP.value = ESP32_IP;

// ============================================================
//  INIT
// ============================================================
pollStatus();
setInterval(pollStatus, POLL_MS);
setInterval(tick, TICK_MS);