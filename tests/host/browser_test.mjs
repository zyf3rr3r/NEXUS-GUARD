import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve} from 'node:path';
import {setTimeout as wait} from 'node:timers/promises';

// This server is a host-only browser fixture. It substitutes every device reading and auth response.
const directory = resolve('build/host');
const profile = resolve(directory, `browser-profile-${process.pid}`);
const edge = process.argv[2] || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const assets = new Map(await Promise.all([
  ['/', 'index.html', 'text/html'], ['/style.css', 'style.css', 'text/css'],
  ['/app.js', 'app.js', 'text/javascript']
].map(async ([route, file, type]) => [route, {content:await readFile(`web/${file}`), type}])));

let sequence = 0;
let pollSeconds = 3;
let failTelemetry = false;
let jobState = 'queued';
let failCancel = false;
let jobStatusRequests = 0;
let locked = true;
const fixtureCode = '1'.repeat(32);
const fixtureToken = '2'.repeat(32);
const server = createServer(async (req, res) => {
  const asset = assets.get(req.url);
  if (asset) {
    res.writeHead(200, {'content-type':asset.type, 'content-security-policy':
      "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"});
    res.end(asset.content);
    return;
  }
  const json = (data, status = 200) => {
    res.writeHead(status, {'content-type':'application/json', 'cache-control':'no-store'});
    res.end(JSON.stringify(data));
  };
  const error = (status, code) => json({error:{code}}, status);
  const route = req.url.slice('/api/v1'.length);
  const body = [];
  for await (const chunk of req) body.push(chunk);
  const text = Buffer.concat(body).toString();
  if (req.method !== 'GET' && req.headers.origin !== `http://127.0.0.1:${server.address().port}`)
    return error(403, 'ORIGIN_REJECTED');
  if (route === '/auth/session' && req.method === 'POST') {
    if (text !== fixtureCode || !locked) return error(401, 'INVALID_OPERATOR_CODE');
    locked = false;
    return json({token:fixtureToken, expiresInSeconds:300});
  }
  if ((req.method !== 'GET' || route.startsWith('/jobs/')) &&
      (locked || req.headers.authorization !== `Bearer ${fixtureToken}`)) return error(401, 'AUTH_REQUIRED');
  if (route === '/telemetry') {
    if (failTelemetry) return error(503, 'FIXTURE_DISCONNECTED');
    return json({
      sequence:++sequence, uptimeMs:81000, cpu:{frequencyMHz:160, cores:2}, temperature:{valueC:32.3},
      flashBytes:16777216, memory:{internalFreeBytes:245760, internalLargestBlockBytes:196608,
        internalMinimumFreeBytes:237568, psramTotalBytes:8388608, psramFreeBytes:8355840,
        psramLargestBlockBytes:8192000, psramMinimumFreeBytes:8323072},
      storage:{mounted:false}, guard:{state:'running', warning:'NONE', reason:'NONE'}, pollSeconds
    });
  }
  if (route === '/system') return json({firmwareVersion:'host fixture', buildId:'fixture-only', resetReason:1});
  if (route === '/apps') return json([
    {id:'pocketos', name:'PocketOS', state:'available'}, {id:'chipvitals', name:'ChipVitals', state:'available'},
    {id:'settings', name:'Settings', state:'available'}, {name:'AirScope', state:'not implemented', detail:'Wi-Fi discovery'},
    {name:'BlueScope', state:'not implemented', detail:'BLE discovery'}
  ]);
  if (route === '/settings' && req.method === 'PATCH') {
    if (!['2','3','5'].includes(text)) return error(400, 'INVALID_POLL_INTERVAL');
    pollSeconds = Number(text);
    return json({applied:true});
  }
  if (route === '/settings') return json({ssid:'NEXUS-S3', channel:1, clientLimit:1, txPowerDbm:8.5, pollSeconds});
  if (route === '/events') return json([{uptimeMs:20000,state:'running',reason:'NONE'}]);
  if (route === '/services/management/stop' && req.method === 'POST') {
    jobState = 'queued';
    return json({id:12, accepted:true, remainingMs:15000}, 202);
  }
  if (route === '/jobs/12/cancel' && req.method === 'POST') {
    if (failCancel) return error(503, 'FIXTURE_INTERRUPTED_CANCEL');
    jobState = 'cancelled';
    return json({id:12, state:jobState});
  }
  if (route === '/jobs/12') {
    ++jobStatusRequests;
    return json({id:12, state:jobState});
  }
  if (route === '/auth/session' && req.method === 'DELETE') {
    locked = true;
    return json({ended:true});
  }
  return error(404, 'UNKNOWN_ROUTE');
});

async function until(check, description) {
  const expires = Date.now() + 6000;
  do {
    const result = await check();
    if (result) return result;
    await wait(100);
  } while (Date.now() < expires);
  throw new Error(`Timed out waiting for ${description}`);
}

await mkdir(directory, {recursive:true});
await new Promise((accept, reject) => {
  server.once('error', reject);
  server.listen(0, '127.0.0.1', accept);
});
const browser = spawn(edge, ['--headless=new', '--disable-gpu', '--disable-background-networking',
  '--disable-component-update', '--no-first-run', '--no-default-browser-check',
  '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'], {stdio:'ignore'});
let launchError;
browser.once('error', error => { launchError = error; });
let socket;
try {
  const port = await until(async () => {
    if (launchError) throw launchError;
    if (browser.exitCode !== null) throw new Error(`Browser exited with ${browser.exitCode}`);
    try { return (await readFile(resolve(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; }
    catch { return null; }
  }, 'browser debugger');
  const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  socket = new WebSocket(pages.find(page => page.type === 'page').webSocketDebuggerUrl);
  await new Promise((accept, reject) => {
    socket.addEventListener('open', accept, {once:true});
    socket.addEventListener('error', reject, {once:true});
  });
  let id = 0;
  const pending = new Map();
  socket.addEventListener('message', ({data}) => {
    const msg = JSON.parse(data);
    pending.get(msg.id)?.(msg);
  });
  async function send(method, params = {}) {
    const request = ++id;
    try {
      const reply = await Promise.race([
        new Promise(accept => {
          pending.set(request, accept);
          socket.send(JSON.stringify({id:request, method, params}));
        }),
        wait(6500, null, {ref:false})
      ]);
      if (!reply || reply.error) throw new Error(`${method} failed ${JSON.stringify(reply?.error)}`);
      return reply.result;
    } finally { pending.delete(request); }
  }
  async function evaluate(expression) {
    const {result, exceptionDetails} = await send('Runtime.evaluate', {expression, returnByValue:true});
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description || exceptionDetails.text);
    return result.value;
  }
  const pageReady = (expression, description) => until(() => evaluate(expression), description);
  const resize = (width, height) => send('Emulation.setDeviceMetricsOverride',
    {width, height, deviceScaleFactor:1, mobile:false});
  async function capture(file) {
    const {data} = await send('Page.captureScreenshot', {format:'png'});
    await writeFile(resolve(directory, file), Buffer.from(data, 'base64'));
  }
  await resize(1280, 920);
  await send('Page.navigate', {url:`http://127.0.0.1:${server.address().port}/`});
  await pageReady("document.querySelector('#connection')?.textContent === 'Connected' && document.querySelector('#apps')?.textContent.includes('AirScope')", 'dashboard data');
  assert.equal(await evaluate('document.documentElement.dataset.theme'), 'dark');
  assert.equal(await evaluate("document.querySelector('[data-value=temp]').textContent"), '32.3 °C');
  assert.ok(await evaluate("document.querySelector('#apps').textContent.includes('not implemented')"));
  await capture('pocketos-desktop.png');
  await evaluate("document.querySelector('nav a[href=\x22#chipvitals\x22]').click()");
  await pageReady("!document.getElementById('chipvitals').hidden", 'ChipVitals navigation');
  assert.equal(await evaluate("document.activeElement.textContent"), 'ChipVitals');
  assert.ok(await evaluate("document.querySelector('#history').textContent.includes('32.3 °C')"));

  await resize(360, 800);
  await capture('pocketos-mobile.png');
  assert.ok(await evaluate("document.documentElement.scrollWidth <= document.documentElement.clientWidth"));
  await evaluate("document.querySelector('nav a[href=\x22#settings\x22]').click()");
  await pageReady("!document.getElementById('settings').hidden && document.querySelector('[data-value=network]').textContent.includes('NEXUS-S3')", 'settings data');
  assert.ok(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'));
  await evaluate(`document.getElementById('operator-code').value = '${fixtureCode}'; document.getElementById('login').requestSubmit(document.querySelector('#login button'))`);
  await pageReady("!document.getElementById('controls').hidden", 'operator sign-in');
  assert.equal(await evaluate('document.activeElement.id'), 'interval');
  await evaluate("document.getElementById('interval').value = '5'; document.getElementById('save').requestSubmit(document.querySelector('#save button'))");
  await pageReady("document.getElementById('feedback').textContent.includes('applied')", 'setting saved');
  assert.equal(pollSeconds, 5);
  await evaluate("document.getElementById('stop').click()");
  await pageReady("!document.getElementById('cancel').hidden", 'shutdown job cancellation control');
  assert.ok(await evaluate("document.getElementById('feedback').textContent.includes('15 seconds')"));
  await wait(5200);
  failCancel = true;
  await evaluate("document.getElementById('cancel').click()");
  await pageReady("document.getElementById('feedback').textContent.includes('Try Cancel stop again')", 'failed cancellation retry');
  assert.equal(jobState, 'queued');
  failCancel = false;
  await evaluate("document.getElementById('cancel').click()");
  await pageReady("document.getElementById('feedback').textContent === 'Shutdown cancelled.'", 'shutdown cancelled');
  assert.equal(jobState, 'cancelled');
  assert.equal(jobStatusRequests, 0, 'the cancellation window must not poll job status');
  await evaluate("document.getElementById('logout').click()");
  await pageReady("document.getElementById('controls').hidden", 'controls locked');
  assert.equal(await evaluate('document.activeElement.id'), 'operator-code');
  assert.ok(locked);
  const theme = await evaluate("document.getElementById('theme').click(); localStorage.getItem('nexus-theme')");
  assert.match(theme, /^(light|dark)$/);
  await send('Page.reload');
  await pageReady(`performance.getEntriesByType('navigation')[0]?.type === 'reload' && document.documentElement.dataset.theme === '${theme}'`, 'theme persists across reload');

  failTelemetry = true;
  await evaluate("location.hash = 'pocketos'");
  await pageReady("!document.getElementById('pocketos').hidden", 'overview navigation');
  await evaluate("document.getElementById('refresh').click()");
  await pageReady("!document.getElementById('offline').hidden", 'stale telemetry warning');
  assert.ok(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'));
  failTelemetry = false;
  await evaluate("document.getElementById('refresh').click()");
  await pageReady("document.getElementById('offline').hidden && document.getElementById('connection').textContent === 'Connected'", 'telemetry recovery');
  console.log('PASS desktop/mobile render, registry, telemetry, navigation, login, settings, stop/cancel, logout, theme persistence and stale/reconnect flows (host fixture)');
} finally {
  socket?.close();
  browser.kill();
  server.closeAllConnections();
  server.close();
}
