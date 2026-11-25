'use strict';

const byId = id => document.getElementById(id);
const views = ['pocketos', 'chipvitals', 'settings'];
const historyRows = [];
let interval = 3;
let timer;
let fetching = false;
let failures = 0;
let sequence;
let token = '';
let sessionTimer;
let stopJob;
let jobTimer;
let activeView;

const reasons = {
  NONE: 'None', TEMP_UNAVAILABLE: 'Temperature unavailable',
  THERMAL_ADMISSION_DENIED: 'Device too warm to start',
  THERMAL_CUTOFF: 'Thermal cutoff reached', THERMAL_WARNING: 'Approaching thermal cutoff',
  HEAP_RESERVE: 'Internal heap reserve reached', PSRAM_RESERVE: 'PSRAM reserve reached',
  BLOCK_TOO_SMALL: 'Contiguous memory too small', COOLDOWN: 'Cooling down',
  SESSION_EXPIRED: 'Management window expired', DRIVER_START_FAILED: 'Startup failed',
  DRIVER_STOP_UNCONFIRMED: 'Release unconfirmed. Check UART.',
  STOP_REQUESTED: 'Stop requested', HEAP_INTEGRITY: 'Heap integrity fault',
  INVALID_OPERATOR_CODE: 'Code invalid or already used. Rotate it with K on UART.',
  TRY_LATER: 'Wait two seconds before retrying.',
  AUTH_REQUIRED: 'Controls are locked. Request a fresh code on UART.',
  STOP_ALREADY_RUNNING: 'Stop has already begun. Check UART for confirmation.',
  INVALID_POLL_INTERVAL: 'Choose a supported refresh interval.'
};

const label = value => reasons[value] || value || 'Unavailable';
const bytes = value => Number.isFinite(value) ? value >= 1048576 ?
  `${(value / 1048576).toFixed(2)} MiB` : `${(value / 1024).toFixed(1)} KiB` : 'Unavailable';
const temp = value => Number.isFinite(value) ? `${value.toFixed(1)} °C` : 'Unavailable';
function uptime(ms) {
  if (!Number.isFinite(ms)) return 'Unavailable';
  const seconds = Math.floor(ms / 1000);
  const minutes = Math.floor(seconds / 60);
  const hours = Math.floor(minutes / 60);
  return hours ? `${hours}h ${minutes % 60}m` : `${minutes}m ${seconds % 60}s`;
}
function value(field, text) {
  document.querySelectorAll(`[data-value="${field}"]`).forEach(element => { element.textContent = text; });
}
function cell(tag, text, className) {
  const element = document.createElement(tag);
  element.textContent = text;
  if (className) element.className = className;
  return element;
}
function feedback(message) { byId('feedback').textContent = message; }

async function api(route, options = {}) {
  const headers = {};
  if (options.body !== undefined) headers['Content-Type'] = 'text/plain';
  if (options.auth && token) headers.Authorization = `Bearer ${token}`;
  const response = await fetch(`/api/v1${route}`, {
    method: options.method || 'GET', body: options.body, headers, cache: 'no-store',
    credentials: 'omit', signal: AbortSignal.timeout(4100)
  });
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401 && options.auth) setSession('');
    const error = new Error(label(data.error?.code || 'Request failed.'));
    error.code = data.error?.code;
    throw error;
  }
  return data;
}

function showView(moveFocus = false) {
  const page = location.hash.slice(1);
  const active = views.includes(page) ? page : views[0];
  for (const id of views) byId(id).hidden = id !== active;
  for (const link of document.querySelectorAll('nav a')) {
    if (link.hash === `#${active}`) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  if (moveFocus && activeView !== active) byId(active).querySelector('h1').focus();
  activeView = active;
  if (active === 'settings') loadSettings().catch(() => feedback('Device settings are temporarily unavailable.'));
}

function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  byId('theme').textContent = theme === 'dark' ? 'Light theme' : 'Dark theme';
}
let theme = 'dark';
try { if (localStorage.getItem('nexus-theme') === 'light') theme = 'light'; } catch {}
applyTheme(theme);
byId('theme').addEventListener('click', () => {
  theme = theme === 'dark' ? 'light' : 'dark';
  applyTheme(theme);
  try { localStorage.setItem('nexus-theme', theme); } catch {}
});

function renderTelemetry(data) {
  const memory = data.memory;
  value('temp', temp(data.temperature.valueC));
  value('heap', bytes(memory.internalFreeBytes));
  value('psram', bytes(memory.psramFreeBytes));
  value('uptime', uptime(data.uptimeMs));
  value('state', data.guard.state);
  value('reason', label(data.guard.reason));
  value('warning', label(data.guard.warning));
  value('cpu', `${data.cpu.frequencyMHz} MHz · ${data.cpu.cores} cores`);
  value('flash', bytes(data.flashBytes));
  value('storage', data.storage.mounted ? bytes(data.storage.freeBytes) : 'Unmounted');
  value('heapLargest', bytes(memory.internalLargestBlockBytes));
  value('heapMin', bytes(memory.internalMinimumFreeBytes));
  value('psramTotal', bytes(memory.psramTotalBytes));
  value('psramLargest', bytes(memory.psramLargestBlockBytes));
  value('psramMin', bytes(memory.psramMinimumFreeBytes));
  interval = [2, 3, 5].includes(data.pollSeconds) ? data.pollSeconds : 3;
  const clock = new Date().toLocaleTimeString();
  byId('last-seen').textContent = `Last received at ${clock}`;
  if (data.sequence === sequence) return;
  if (sequence !== undefined && data.sequence < sequence) historyRows.length = 0;
  sequence = data.sequence;
  const row = document.createElement('tr');
  for (const text of [clock, temp(data.temperature.valueC), bytes(memory.internalFreeBytes), bytes(memory.psramFreeBytes)]) {
    row.append(cell('td', text));
  }
  historyRows.unshift(row);
  if (historyRows.length > 24) historyRows.pop();
  byId('history').replaceChildren(...historyRows);
}

let catalogLoaded = false;
async function loadCatalog() {
  const apps = await api('/apps');
  const list = apps.map(app => {
    const row = document.createElement('li');
    const available = app.state === 'available' && views.includes(app.id);
    const name = cell(available ? 'a' : 'strong', app.name);
    if (available) name.href = `#${app.id}`;
    row.append(name, cell('span', app.detail || '', 'detail'),
      cell('span', app.state, `app-state ${app.state === 'available' ? 'available' : ''}`));
    return row;
  });
  byId('apps').replaceChildren(...list);
  const device = await api('/system');
  value('firmware', device.firmwareVersion);
  value('build', device.buildId);
  value('reset', ({1:'Power-on', 2:'External', 3:'Software restart', 4:'Panic', 5:'Interrupt watchdog',
    6:'Task watchdog', 7:'Watchdog', 8:'Deep sleep', 9:'Brownout', 10:'SDIO'})[device.resetReason] || `SDK code ${device.resetReason}`);
  catalogLoaded = true;
}

async function poll() {
  clearTimeout(timer);
  if (fetching || document.hidden) return;
  fetching = true;
  const connection = byId('connection');
  try {
    renderTelemetry(await api('/telemetry'));
    connection.dataset.state = 'live';
    if (connection.textContent !== 'Connected') connection.textContent = 'Connected';
    byId('offline').hidden = true;
    failures = 0;
    if (!catalogLoaded) await loadCatalog().catch(() => {});
  } catch {
    failures++;
    connection.dataset.state = 'stale';
    if (connection.textContent !== 'Reconnecting') connection.textContent = 'Reconnecting';
    byId('offline').hidden = false;
  } finally {
    fetching = false;
    if (!document.hidden) timer = setTimeout(poll, 1000 * Math.min(30, interval * 2 ** failures));
  }
}

async function loadSettings() {
  const settings = await api('/settings');
  byId('interval').value = String(settings.pollSeconds);
  value('network', `${settings.ssid} · ch ${settings.channel}`);
  value('limit', String(settings.clientLimit));
  value('power', `${settings.txPowerDbm} dBm`);
  const events = await api('/events');
  const rows = events.map(event => cell('li', `${uptime(event.uptimeMs)} · ${event.state} · ${label(event.reason)}`));
  byId('events').replaceChildren(...(rows.length ? rows : [cell('li', 'No recorded state changes.')]));
}

function setSession(next, seconds, moveFocus = false) {
  clearTimeout(sessionTimer);
  const controlsFocused = byId('controls').contains(document.activeElement);
  token = next;
  byId('login').hidden = Boolean(token);
  byId('controls').hidden = !token;
  if (!byId('settings').hidden && (moveFocus || (!token && controlsFocused)))
    byId(token ? 'interval' : 'operator-code').focus();
  if (token) sessionTimer = setTimeout(() => {
    setSession('');
    feedback('Operator session expired. Rotate the code on UART to unlock controls again.');
  }, seconds * 1000);
}
async function perform(button, operation) {
  button.disabled = true;
  try { await operation(); } catch (error) { feedback(error.message); }
  finally { button.disabled = false; }
}

byId('login').addEventListener('submit', event => {
  event.preventDefault();
  const input = byId('operator-code');
  const code = input.value.trim();
  input.value = '';
  if (!/^[0-9a-f]{32}$/.test(code)) { feedback('Enter the 32-character operator code from UART.'); return; }
  perform(event.submitter, async () => {
    const session = await api('/auth/session', {method:'POST', body:code});
    setSession(session.token, session.expiresInSeconds, true);
    feedback('Controls unlocked for five minutes.');
  });
});
byId('save').addEventListener('submit', event => {
  event.preventDefault();
  const selected = byId('interval').value;
  perform(event.submitter, async () => {
    await api('/settings', {method:'PATCH', auth:true, body:selected});
    interval = Number(selected);
    feedback('Refresh interval applied for this boot.');
  });
});
byId('logout').addEventListener('click', event => perform(event.currentTarget, async () => {
  await api('/auth/session', {method:'DELETE', auth:true});
  setSession('', 0, true);
  feedback('Controls locked.');
}));
function finishStopJob(job, message) {
  if (stopJob !== job) return;
  clearTimeout(jobTimer);
  stopJob = undefined;
  byId('cancel').hidden = true;
  byId('stop').disabled = false;
  feedback(message);
}
function watchStopDeadline(job) {
  jobTimer = setTimeout(() =>
    finishStopJob(job, 'Shutdown window ended. Check UART for confirmation.'),
  Math.max(0, job.deadline - performance.now()));
}
byId('stop').addEventListener('click', async event => {
  const button = event.currentTarget;
  button.disabled = true;
  try {
    const response = await api('/services/management/stop', {method:'POST', auth:true});
    const remaining = Number.isFinite(response.remainingMs) ? Math.max(0, response.remainingMs) : 0;
    const job = {id:response.id, deadline:performance.now() + remaining};
    stopJob = job;
    if (remaining === 0) {
      finishStopJob(job, 'Shutdown is due. Check UART for confirmation.');
      return;
    }
    byId('cancel').hidden = false;
    feedback(`Shutdown queued. Cancel within about ${Math.ceil(remaining / 1000)} seconds.`);
    watchStopDeadline(job);
  } catch (error) {
    button.disabled = false;
    feedback(error.message);
  }
});
byId('cancel').addEventListener('click', async event => {
  const job = stopJob;
  if (!job) return;
  const button = event.currentTarget;
  button.disabled = true;
  clearTimeout(jobTimer);
  feedback('Cancelling shutdown…');
  try {
    const result = await api(`/jobs/${job.id}/cancel`, {method:'POST', auth:true});
    if (result.state !== 'cancelled') throw new Error('Cancellation was not confirmed.');
    finishStopJob(job, 'Shutdown cancelled.');
  } catch (error) {
    if (token && error.code !== 'STOP_ALREADY_RUNNING' && performance.now() < job.deadline) {
      feedback('Cancellation was not confirmed. Try Cancel stop again before the window ends.');
      watchStopDeadline(job);
    } else {
      finishStopJob(job, 'Cancellation was not confirmed. Check UART for the stop result.');
    }
  } finally {
    button.disabled = false;
  }
});
byId('refresh').addEventListener('click', poll);
addEventListener('hashchange', () => showView(true));
document.addEventListener('visibilitychange', () => document.hidden ? clearTimeout(timer) : poll());
showView();
poll();
