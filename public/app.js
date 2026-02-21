'use strict';

// ── DOM ──
const lens      = document.getElementById('lens');
const beam      = document.getElementById('beam');
const floorGlow = document.getElementById('floorGlow');
const stateTxt  = document.getElementById('stateTxt');
const btnOn     = document.getElementById('btnOn');
const btnOff    = document.getElementById('btnOff');
const dot       = document.getElementById('dot');
const statusMsg = document.getElementById('statusMsg');

// ── State ──
let isOn = false;
let busy = false;

// ============================================================
//  APPLY VISUAL STATE
// ============================================================
function applyVisual(on) {
  isOn = on;

  // Fixture
  lens.className      = 'lens'       + (on ? ' on' : '');
  beam.className      = 'beam'       + (on ? ' on' : '');
  floorGlow.className = 'floor-glow' + (on ? ' on' : '');

  // Text
  stateTxt.textContent = on ? 'ON' : 'OFF';
  stateTxt.className   = 'state-txt' + (on ? ' on' : '');

  // Button active states
  btnOn.classList.toggle('active',  on);
  btnOff.classList.toggle('active', !on);
}

// ============================================================
//  STATUS INDICATOR
// ============================================================
function setStatus(msg, state) {
  // state: 'online' | 'error' | 'busy'
  statusMsg.textContent = msg;
  dot.className = 'dot ' + (state === 'online' ? 'online' : state === 'error' ? 'error' : '');
}

// ============================================================
//  SEND COMMAND — POST through server.js proxy
// ============================================================
function sendCommand(desired) {
  if (busy) return;
  if (isOn === desired) return;

  busy = true;
  btnOn.disabled  = true;
  btnOff.disabled = true;
  setStatus('Sending...', 'busy');

  fetch(`/proxy/set?state=${desired ? 1 : 0}`, {
    method: 'POST',
    signal: AbortSignal.timeout(6000)
  })
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    })
    .then(data => {
      applyVisual(data.state);
      setStatus(data.state ? 'Light is ON' : 'Light is OFF', 'online');
    })
    .catch(() => {
      setStatus('Device unreachable', 'error');
    })
    .finally(() => {
      busy = false;
      btnOn.disabled  = false;
      btnOff.disabled = false;
    });
}

// ============================================================
//  POLL STATUS — sync on load and every 5s
// ============================================================
function pollStatus() {
  fetch('/proxy/status', { signal: AbortSignal.timeout(4000) })
    .then(r => r.json())
    .then(data => {
      applyVisual(data.state);
      setStatus(data.state ? 'Light is ON' : 'Light is OFF', 'online');
    })
    .catch(() => {
      setStatus('Device unreachable', 'error');
    });
}

// ============================================================
//  BUTTON LISTENERS
// ============================================================
btnOn.addEventListener('click',  () => sendCommand(true));
btnOff.addEventListener('click', () => sendCommand(false));

// ============================================================
//  INIT
// ============================================================
pollStatus();
setInterval(pollStatus, 5000);