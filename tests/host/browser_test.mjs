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
  ['/app.js', 'app.js', 'text/javascript'], ['/apps.js', 'apps.js', 'text/javascript']
].map(async ([route, file, type]) => [route, {content:await readFile(`web/${file}`), type}])));

let sequence = 0;
let pollSeconds = 3;
let failTelemetry = false;
let jobState = 'queued';
let failCancel = false;
let jobStatusRequests = 0;
let locked = true;
let radioKind = 'wifi';
let denyRadio = false;
const inbox = [];
const fixtureCode = '1'.repeat(32);
const fixtureToken = '2'.repeat(32);
const server = createServer(async (req, res) => {
  const asset = assets.get(new URL(req.url, 'http://127.0.0.1').pathname);
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
  if ((req.method !== 'GET' || route.startsWith('/jobs/') || ['/radio','/inbox'].includes(route)) &&
      (locked || req.headers.authorization !== `Bearer ${fixtureToken}`)) return error(401, 'AUTH_REQUIRED');
  if (route === '/telemetry') {
    if (failTelemetry) return error(503, 'FIXTURE_DISCONNECTED');
    return json({
      sequence:++sequence, uptimeMs:81000, cpu:{frequencyMHz:160, cores:2}, temperature:{valueC:32.3, valid:true},
      flashBytes:16777216, memory:{internalFreeBytes:245760, internalLargestBlockBytes:196608,
        internalMinimumFreeBytes:237568, psramTotalBytes:8388608, psramFreeBytes:8355840,
        psramLargestBlockBytes:8192000, psramMinimumFreeBytes:8323072},
      storage:{mounted:false}, guard:{state:'running', warning:'NONE', reason:'NONE'}, pollSeconds
    });
  }
  if (route === '/system') return json({chip:'ESP32-S3', firmwareVersion:'host fixture', buildId:'fixture-only', resetReason:1});
  if (route === '/apps') return json([
    {id:'pocketos', name:'PocketOS', state:'available'}, {id:'chipvitals', name:'ChipVitals', state:'available'},
    {id:'settings', name:'Settings', state:'available'}, {id:'airscope', name:'AirScope', state:'available', detail:'Wi-Fi discovery'},
    {id:'bluescope', name:'BlueScope', state:'available', detail:'BLE discovery'}
  ]);
  if (route === '/guard') return json({state:'running', warning:'NONE', heapReserveBytes:65536});
  if (['/radio/wifi','/radio/ble','/radio/csi'].includes(route)) {
    if (denyRadio) return error(409, 'RADIO_THERMAL_MARGIN');
    radioKind = route.split('/').at(-1);
    return json({queued:true}, 202);
  }
  if (route === '/radio') return json({kind:radioKind, state:'done', frames:16, mean:17, minimum:11, maximum:22,
    rows:[{name:'Fixture radio',channel:1,rssi:-45,security:3}]});
  if (route === '/inbox' && req.method === 'POST') {
    inbox.push({id:inbox.length+1,text});
    return json({saved:true}, 201);
  }
  if (route === '/inbox') return json(inbox);
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
  '--disable-component-update', '--no-first-run', '--no-default-browser-check', '--autoplay-policy=no-user-gesture-required',
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
  assert.ok(await evaluate("Array.from(document.querySelectorAll('#apps a')).some(link => link.hash === '#airscope')"));
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

  const visit = async (id, title) => {
    await evaluate(`location.hash = ${JSON.stringify(id)}`);
    await pageReady(`document.getElementById('application-name').textContent === ${JSON.stringify(title)}`, title);
  };
  const clickApp = text => evaluate(`Array.from(document.querySelectorAll('#application-body button')).find(button => button.textContent === ${JSON.stringify(text)}).click()`);
  const appHas = text => `document.getElementById('application-body').textContent.includes(${JSON.stringify(text)})`;
  for (const [id, title, button, result] of [
    ['airscope','AirScope','Scan','channel 1'],
    ['bluescope','BlueScope','Scan','Fixture radio'],
    ['ghostsense','GhostSense','Capture link','Frames: 16'],
    ['cyberdeck','CyberDeck','Run checks','Checks complete.']
  ]) {
    await visit(id, title);
    await clickApp(button);
    await pageReady(appHas(result), `${title} result`);
  }
  await visit('airscope','AirScope');
  denyRadio = true;
  await clickApp('Scan');
  await pageReady(appHas('Guard needs more thermal headroom.'), 'AirScope thermal denial');
  denyRadio = false;
  radioKind = 'wifi';
  await clickApp('Load last scan');
  await pageReady(appHas('channel 1'), 'load a previously captured Wi-Fi scan');
  await visit('deaddrop','DeadDrop');
  await evaluate("document.querySelector('#application-body input').value = 'Local fixture note'");
  await clickApp('Send note');
  await pageReady(appHas('Local fixture note'), 'inbox note');
  await visit('toolbox','Toolbox');
  await evaluate("document.querySelector('#application-body textarea').value = 'NEXUS'");
  await clickApp('Transform');
  await pageReady(appHas('TkVYVVM='), 'Base64 transform');
  await visit('pocketvault','PocketVault');
  await evaluate("document.querySelector('#application-body input').value = 'fixture-vault-passphrase'; document.querySelector('#application-body textarea').value = 'Encrypted fixture note'");
  await clickApp('Encrypt note');
  await pageReady(appHas('Note encrypted.'), 'vault encrypt');
  assert.ok(await evaluate("!document.querySelectorAll('#application-body textarea')[1].value.includes('Encrypted fixture note')"));
  await evaluate("document.querySelector('#application-body input').value = 'fixture-vault-passphrase'");
  await clickApp('Decrypt note');
  await pageReady(appHas('Note decrypted'), 'vault decrypt');
  assert.equal(await evaluate("document.querySelector('#application-body textarea').value"), 'Encrypted fixture note');
  await evaluate(`
    const fields = document.querySelectorAll('#application-body textarea');
    const envelope = JSON.parse(fields[1].value);
    envelope.data = (envelope.data[0] === 'A' ? 'B' : 'A') + envelope.data.slice(1);
    fields[1].value = JSON.stringify(envelope);
    document.querySelector('#application-body input').value = 'fixture-vault-passphrase';
  `);
  await clickApp('Decrypt note');
  await pageReady(appHas('did not match'), 'vault rejects a tampered envelope');
  await visit('soundforge','SoundForge');
  await clickApp('Play sequence');
  await pageReady(appHas('Playing through this browser'), 'audio sequencer');
  await clickApp('Stop playback');
  await pageReady(appHas('Playback stopped.'), 'audio stop');
  await evaluate(`
    window.midiHeader = null;
    const makeDownloadUrl = URL.createObjectURL;
    URL.createObjectURL = blob => {
      blob.arrayBuffer().then(bytes => { window.midiHeader = Array.from(new Uint8Array(bytes, 0, 14)); });
      return makeDownloadUrl(blob);
    };
    const clickLink = HTMLAnchorElement.prototype.click;
    HTMLAnchorElement.prototype.click = function () {
      if (!this.download) clickLink.call(this);
    };
  `);
  await clickApp('Download MIDI');
  await pageReady('window.midiHeader !== null', 'MIDI export');
  assert.deepEqual(await evaluate('window.midiHeader'), [77,84,104,100,0,0,0,6,0,0,0,1,1,224]);
  await visit('portalplay','PortalPlay');
  assert.equal(await evaluate("document.querySelectorAll('.game-grid button').length"), 16);
  await evaluate("document.querySelector('.game-grid button').click()");
  assert.notEqual(await evaluate("document.querySelector('.game-grid button').textContent"), '?');
  await visit('cyberarena','CyberArena');
  for (const answer of ['NEXUS','Quarantine it and investigate','Read the required log']) await clickApp(answer);
  assert.ok(await evaluate(appHas('All three exercises complete.')));
  for (const [id, title] of [['usb-studio','USB Studio'],['ghostlink','GhostLink']]) {
    await visit(id, title);
    assert.ok(await evaluate(appHas('Choose serial port') + ' || ' + appHas('supported browser')));
  }
  await evaluate(`
    window.serialWrites = [];
    Object.defineProperty(navigator, 'serial', { configurable:true, value:{
      requestPort:async () => ({
        open:async () => {},
        close:async () => {},
        readable:{getReader:() => ({
          read:() => new Promise(resolve => { window.endFixtureRead = resolve; }),
          cancel:async () => window.endFixtureRead({done:true}), releaseLock:() => {}
        })},
        writable:{getWriter:() => ({
          write:async bytes => window.serialWrites.push(new TextDecoder().decode(bytes)),
          releaseLock:() => {}
        })}
      })
    } })
  `);
  await visit('portalplay','PortalPlay');
  await visit('ghostlink','GhostLink');
  await clickApp('Choose serial port');
  await pageReady(appHas('Serial port open'), 'paired mock USB port');
  await clickApp('Play / pause');
  await pageReady(appHas('Pair and arm'), 'media blocked before arm');
  assert.deepEqual(await evaluate('window.serialWrites'), []);
  await clickApp('Arm one command');
  await pageReady(appHas('Arm request sent'), 'media arming');
  await clickApp('Play / pause');
  await pageReady(appHas('Command sent.'), 'paired media action');
  assert.deepEqual(await evaluate('window.serialWrites'), ['U','P']);
  await clickApp('Play / pause');
  await pageReady(appHas('Pair and arm'), 'media arming used once');
  await clickApp('Disconnect');
  await pageReady(appHas('Serial port closed'), 'mock USB disconnection');
  await evaluate("location.hash = 'settings'");
  await pageReady("!document.getElementById('settings').hidden", 'back to Settings for sign-out');
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
  await send('Page.navigate', {url:`http://127.0.0.1:${server.address().port}/?offline=1#toolbox`});
  await pageReady("document.getElementById('connection')?.textContent === 'Local preview'", 'offline preview');
  await pageReady("document.getElementById('application-name')?.textContent === 'Toolbox'", 'offline app route');
  console.log('PASS desktop/mobile render, registry, telemetry, navigation, login, settings, stop/cancel, logout, theme persistence and stale/reconnect flows (host fixture)');
  console.log('PASS twelve app views, fixture radio and inbox, browser encryption round trip, transforms, audio, game, and quiz (host fixture)');
} finally {
  socket?.close();
  browser.kill();
  server.closeAllConnections();
  server.close();
}
