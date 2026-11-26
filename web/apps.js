'use strict';

(() => {
  const catalog = [
    ['airscope', 'AirScope', 'Passive Wi-Fi discovery'],
    ['bluescope', 'BlueScope', 'Passive BLE discovery'],
    ['cyberdeck', 'CyberDeck', 'Diagnostics for this NEXUS-S3'],
    ['deaddrop', 'DeadDrop', 'Local session inbox'],
    ['pocketvault', 'PocketVault', 'Encrypted local notes'],
    ['toolbox', 'Toolbox', 'Offline text utilities'],
    ['usb-studio', 'USB Studio', 'Browser serial monitor'],
    ['ghostlink', 'GhostLink', 'Armed serial shortcuts'],
    ['soundforge', 'SoundForge', 'Browser tone sequencer'],
    ['portalplay', 'PortalPlay', 'Offline memory game'],
    ['ghostsense', 'GhostSense', 'Experimental signal capture'],
    ['cyberarena', 'CyberArena', 'Offline defensive puzzles']
  ].map(([id, name, detail]) => ({id, name, detail, state:'available'}));
  const localApps = new Set(['bluescope', 'pocketvault', 'toolbox', 'usb-studio', 'ghostlink', 'soundforge', 'portalplay', 'cyberarena']);
  const loopback = offlinePreview;
  const body = byId('application-body');
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  let dispose = () => {};

  function node(tag, text = '', className = '') {
    const element = document.createElement(tag);
    element.textContent = text;
    if (className) element.className = className;
    return element;
  }
  function panel(title, description) {
    const element = node('section', '', 'panel app-panel');
    element.append(node('h2', title));
    if (description) element.append(node('p', description, 'caption'));
    body.append(element);
    return element;
  }
  function field(label, tag = 'input') {
    const caption = node('label', label, 'app-field');
    const control = node(tag);
    caption.append(control);
    return [caption, control];
  }
  function button(title, handler) {
    const control = node('button', title);
    control.type = 'button';
    control.addEventListener('click', handler);
    return control;
  }
  function status(parent) {
    const element = node('p', '', 'app-status');
    element.setAttribute('role', 'status');
    parent.append(element);
    return element;
  }
  function action(title, feedback, handler) {
    return button(title, async event => {
      const control = event.currentTarget;
      control.disabled = true;
      try { await handler(); }
      catch (error) { feedback.textContent = error.message || 'The operation failed.'; }
      finally { control.disabled = false; }
    });
  }
  function output(parent) {
    const element = node('pre', '', 'app-output');
    parent.append(element);
    return element;
  }
  function requireDevice() {
    if (loopback) {
      panel('Board connection required', 'Open this app in PocketOS on the ESP32-S3 to access its local service.');
      return false;
    }
    return true;
  }
  function requireOperator() {
    if (token) return true;
    const element = panel('Operator session required', 'Unlock controls in Settings using the one-time UART code, then return to this app.');
    const link = node('a', 'Open Settings', 'text-action');
    link.href = '#settings';
    element.append(link);
    return false;
  }

  function radio(kind) {
    if (!requireDevice() || !requireOperator()) return;
    const wireless = kind === 'wifi';
    const csi = kind === 'csi';
    const panelTitle = wireless ? 'Nearby Wi-Fi networks' : csi ? 'Owned-link CSI experiment' : 'Nearby BLE advertisements';
    const description = wireless ?
      'Run one passive scan, or load a scan queued with W on UART. The connection may pause while the radio visits other channels.' : csi ?
      'Capture three seconds of aggregate signal measurements from the connected client. Compare a baseline with a later sample.' :
      'Run one short passive discovery scan. Names and signal strength are displayed when advertised.';
    const element = panel(panelTitle, description);
    const feedback = status(element);
    const results = output(element);
    let active = false;
    let closed = false;
    let baseline;
    const reasons = {
      RADIO_THERMAL_MARGIN: 'Guard needs more thermal headroom. Stop management and let the board cool before another scan.',
      RADIO_MEMORY_MARGIN: 'Guard found insufficient memory headroom.',
      RADIO_TEMP_UNAVAILABLE: 'Guard could not read the temperature.',
      RADIO_MANAGEMENT_REQUIRED: 'Start management on UART first.',
      RADIO_BUSY: 'Another radio request is active.',
      RADIO_COOLDOWN: 'Wait for the 12-second radio request interval before starting another scan.',
      RADIO_DRIVER_UNAVAILABLE: 'The radio app driver is unavailable. Check UART.',
      RADIO_RELEASE_UNCONFIRMED: 'Radio release was unconfirmed. Stop management on UART.',
      RADIO_START_FAILED: 'The radio could not start. Check UART for the SDK error.',
      RADIO_SCAN_FAILED: 'The scan failed. Check UART for the SDK result.',
      RADIO_TIMEOUT: 'The radio operation reached its time limit.',
      RADIO_STOPPED: 'Management stopped the capture.',
      RADIO_USER_CANCELLED: 'Capture cancelled.'
    };
    function renderRadio(sample) {
      if (sample.kind !== kind || sample.state === 'idle') {
        feedback.textContent = 'No capture for this app in the current management session.';
        return;
      }
      if (['queued','running'].includes(sample.state)) {
        feedback.textContent = 'Capture in progress.';
        return;
      }
      if (sample.state !== 'done') {
        results.textContent = '';
        feedback.textContent = reasons[sample.reason] || 'The radio could not complete this request.';
        return;
      }
      if (csi) {
        if (sample.frames === 0) {
          feedback.textContent = 'No eligible frames were captured.';
        } else {
          const comparison = baseline === undefined ? 'Baseline saved in this view.' :
            `Mean difference from baseline: ${sample.mean - baseline}`;
          if (baseline === undefined) baseline = sample.mean;
          results.textContent = `Frames: ${sample.frames}\nMean amplitude: ${sample.mean}\nRange: ${sample.minimum}–${sample.maximum}\n${comparison}`;
          feedback.textContent = 'Capture complete.';
        }
        return;
      }
      const security = ['Open','WEP','WPA','WPA2','WPA/WPA2','WPA Enterprise','WPA3','WPA2/WPA3'];
      results.textContent = sample.rows.map(row => {
        const signal = row.rssi === 127 ? 'RSSI unavailable' : `${row.rssi} dBm`;
        return wireless ?
          `${row.name || 'Unnamed network'}  ·  channel ${row.channel}  ·  ${signal}  ·  ${security[row.security] || `Mode ${row.security}`}` :
          `${row.name || 'Unnamed advertiser'}  ·  ${signal}`;
      }).join('\n');
      feedback.textContent = sample.rows.length ? `${sample.rows.length} results.` : 'No results were received.';
    }
    const scans = button(csi ? 'Capture link' : 'Scan', async () => {
      scans.disabled = true;
      try {
        await api(`/radio/${kind}`, {method:'POST', auth:true});
        active = true;
        cancel.hidden = false;
        feedback.textContent = 'Request queued. Guard checks the board before starting.';
        for (let attempt = 0; attempt < 22 && !closed; ++attempt) {
          await new Promise(resolve => setTimeout(resolve, csi ? 230 : 450));
          if (closed) break;
          try {
            const sample = await api('/radio', {auth:true});
            if (sample.kind !== kind || ['queued','running'].includes(sample.state)) continue;
            renderRadio(sample);
            return;
          } catch (error) {
            if (!token) throw error;
          }
        }
        if (!closed) feedback.textContent = 'Results were unavailable. Check the device connection and Guard status.';
      } catch (error) {
        feedback.textContent = reasons[error.code] || error.message;
      } finally {
        active = false;
        cancel.hidden = true;
        scans.disabled = false;
      }
    });
    const cancel = action('Cancel', feedback, async () => {
      await api('/radio/cancel', {method:'POST', auth:true});
      feedback.textContent = 'Cancellation requested.';
    });
    cancel.hidden = true;
    const controls = node('div', '', 'actions');
    controls.append(scans, cancel);
    if (wireless) controls.append(action('Load last scan', feedback, async () => {
      renderRadio(await api('/radio', {auth:true}));
    }));
    element.insertBefore(controls, feedback);
    dispose = () => {
      closed = true;
      if (active && token) api('/radio/cancel', {method:'POST', auth:true}).catch(() => {});
    };
  }

  function cyberdeck() {
    if (!requireDevice()) return;
    const element = panel('Local system checks', 'Check the current board’s management endpoints, Guard response, and memory reserves.');
    const feedback = status(element);
    const results = output(element);
    element.insertBefore(action('Run checks', feedback, async () => {
      const checks = [
        ['Identity', '/system', d => d.chip === 'ESP32-S3' ? d.firmwareVersion : 'Unexpected chip identity'],
        ['Telemetry', '/telemetry', d => d.temperature.valid ? `${d.temperature.valueC} °C · ${d.cpu.frequencyMHz} MHz` : 'Temperature unavailable'],
        ['Guard', '/guard', d => `${d.state} · ${d.warning} · reserve ${d.heapReserveBytes} B`],
        ['AP settings', '/settings', d => `channel ${d.channel} · client limit ${d.clientLimit}`]
      ];
      const lines = [];
      for (const [name, route, check] of checks) {
        const start = performance.now();
        try {
          const data = await api(route);
          lines.push(`${name}: ${check(data)} (${Math.round(performance.now() - start)} ms)`);
        } catch (error) { lines.push(`${name}: ${error.message}`); }
        results.textContent = lines.join('\n');
      }
      feedback.textContent = 'Checks complete.';
    }), feedback);
  }

  function deaddrop() {
    if (!requireDevice() || !requireOperator()) return;
    const element = panel('Board inbox', 'Leave a short note for the next authorized session on this boot. The board keeps the most recent eight notes in memory.');
    const [label, message] = field('Message (96 ASCII characters)');
    message.maxLength = 96;
    element.append(label);
    const controls = node('div', '', 'actions');
    element.append(controls);
    const feedback = status(element);
    const list = node('ol', '', 'list');
    element.append(list);
    async function refresh() {
      const notes = await api('/inbox', {auth:true});
      list.replaceChildren(...notes.map(note => node('li', `#${note.id}  ${note.text}`)));
      feedback.textContent = notes.length ? `${notes.length} saved notes.` : 'Inbox is empty.';
    }
    controls.append(action('Send note', feedback, async () => {
      if (!/^[\x20-\x7e]{1,96}$/.test(message.value) || !message.value.trim()) throw new Error('Enter 1–96 printable ASCII characters.');
      await api('/inbox', {method:'POST', body:message.value, auth:true});
      message.value = '';
      await refresh();
    }), action('Refresh inbox', feedback, refresh));
    refresh().catch(error => { feedback.textContent = error.message; });
  }

  const to64 = bytes => btoa(String.fromCharCode(...bytes));
  const from64 = text => Uint8Array.from(atob(text), char => char.charCodeAt(0));
  const toHex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');

  function bluetoothReader() {
    if (!navigator.bluetooth) {
      panel('Bluetooth access requires a supported browser', 'Open the localhost preview in a browser with Web Bluetooth support to read an owned peripheral.');
      return;
    }
    const element = panel('Authorized GATT read', 'Enter the known service and characteristic UUIDs, then select your device in the browser chooser.');
    const [serviceLabel, service] = field('Service UUID');
    const [characteristicLabel, characteristic] = field('Characteristic UUID');
    service.maxLength = characteristic.maxLength = 36;
    element.append(serviceLabel, characteristicLabel);
    const feedback = status(element);
    const result = output(element);
    let device;
    let closed = false;
    element.insertBefore(action('Choose device and read', feedback, async () => {
      const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
      const primary = service.value.trim().toLowerCase();
      const fieldId = characteristic.value.trim().toLowerCase();
      if (!uuid.test(primary) || !uuid.test(fieldId)) throw new Error('Enter both full UUIDs.');
      device = await navigator.bluetooth.requestDevice({filters:[{services:[primary]}]});
      if (closed) return;
      try {
        const server = await device.gatt.connect();
        if (closed) return;
        const handle = await (await server.getPrimaryService(primary)).getCharacteristic(fieldId);
        const value = await handle.readValue();
        const bytes = new Uint8Array(value.buffer, value.byteOffset, Math.min(value.byteLength, 512));
        result.textContent = toHex(bytes);
        feedback.textContent = `Read ${value.byteLength} bytes from the selected characteristic.`;
      } finally { device.gatt?.disconnect(); }
    }), feedback);
    dispose = () => { closed = true; device?.gatt?.disconnect(); };
  }

  function pocketvault() {
    if (!globalThis.crypto?.subtle) {
      panel('Secure browser required', 'Run the included local web server and open PocketVault at localhost. Encryption and decryption happen in that browser.');
      return;
    }
    const element = panel('Encrypted note', 'Encrypt a note with a passphrase and save its encrypted envelope. Keep the envelope and passphrase separately.');
    const [passLabel, passphrase] = field('Passphrase');
    passphrase.type = 'password';
    passphrase.autocomplete = 'new-password';
    passphrase.maxLength = 256;
    const [noteLabel, note] = field('Plaintext note', 'textarea');
    note.maxLength = 8192;
    const [envelopeLabel, envelope] = field('Encrypted envelope', 'textarea');
    envelope.maxLength = 12000;
    const controls = node('div', '', 'actions');
    element.append(passLabel, noteLabel, envelopeLabel, controls);
    const feedback = status(element);
    const rounds = 600000;
    let verifiedEnvelope;
    async function keyFor(salt) {
      const value = passphrase.value;
      if (value.length < 12) throw new Error('Use a passphrase of at least 12 characters.');
      const material = await crypto.subtle.importKey('raw', encoder.encode(value), 'PBKDF2', false, ['deriveKey']);
      return crypto.subtle.deriveKey({name:'PBKDF2', salt, iterations:rounds, hash:'SHA-256'},
        material, {name:'AES-GCM', length:256}, false, ['encrypt', 'decrypt']);
    }
    controls.append(action('Encrypt note', feedback, async () => {
      const data = encoder.encode(note.value);
      if (data.length === 0 || data.length > 8192) throw new Error('Enter a note between 1 and 8,192 UTF-8 bytes.');
      const salt = crypto.getRandomValues(new Uint8Array(16));
      const iv = crypto.getRandomValues(new Uint8Array(12));
      const key = await keyFor(salt);
      const encrypted = await crypto.subtle.encrypt({name:'AES-GCM', iv}, key, data);
      envelope.value = JSON.stringify({version:1, salt:to64(salt), iv:to64(iv), data:to64(new Uint8Array(encrypted))});
      verifiedEnvelope = envelope.value;
      note.value = '';
      passphrase.value = '';
      feedback.textContent = 'Note encrypted. Save the encrypted envelope.';
    }), action('Decrypt note', feedback, async () => {
      let record;
      try { record = JSON.parse(envelope.value); } catch { throw new Error('Enter a valid encrypted envelope.'); }
      if (record?.version !== 1 || typeof record.salt !== 'string' || record.salt.length !== 24 ||
          typeof record.iv !== 'string' || record.iv.length !== 16 ||
          typeof record.data !== 'string' || record.data.length > 11000)
        throw new Error('Unsupported encrypted envelope.');
      const salt = from64(record.salt);
      const iv = from64(record.iv);
      const encrypted = from64(record.data);
      if (salt.length !== 16 || iv.length !== 12 || encrypted.length < 17 || encrypted.length > 8208)
        throw new Error('Invalid envelope size.');
      const key = await keyFor(salt);
      try {
        note.value = decoder.decode(await crypto.subtle.decrypt({name:'AES-GCM', iv}, key, encrypted));
      } catch { throw new Error('The passphrase or encrypted data did not match.'); }
      envelope.value = JSON.stringify({version:1, salt:record.salt, iv:record.iv, data:record.data});
      verifiedEnvelope = envelope.value;
      passphrase.value = '';
      feedback.textContent = 'Note decrypted in this browser.';
    }), action('Save encrypted envelope', feedback, async () => {
      if (!verifiedEnvelope || verifiedEnvelope !== envelope.value)
        throw new Error('Encrypt a note or authenticate this envelope first.');
      localStorage.setItem('nexus-encrypted-note', verifiedEnvelope);
      feedback.textContent = 'Encrypted envelope saved in this browser.';
    }), action('Load saved envelope', feedback, async () => {
      envelope.value = localStorage.getItem('nexus-encrypted-note') || '';
      feedback.textContent = envelope.value ? 'Encrypted envelope loaded.' : 'No encrypted envelope is saved in this browser.';
    }));
    dispose = () => { note.value = ''; passphrase.value = ''; };
  }

  function toolbox() {
    const element = panel('Text transforms', 'Convert up to 4 KiB of text. SHA-256 requires a secure browser context.');
    const [inputLabel, input] = field('Input', 'textarea');
    input.maxLength = 4096;
    const [operationLabel, operation] = field('Operation', 'select');
    const names = {'base64':'Text to Base64', 'unbase64':'Base64 to text', 'hex':'Text to hex',
      'unhex':'Hex to text', 'url':'URL encode', 'unurl':'URL decode', 'sha':'SHA-256'};
    for (const [id, name] of Object.entries(names)) {
      const option = node('option', name);
      option.value = id;
      operation.append(option);
    }
    element.append(inputLabel, operationLabel);
    const feedback = status(element);
    const result = output(element);
    element.insertBefore(action('Transform', feedback, async () => {
      const text = input.value;
      const data = encoder.encode(text);
      if (data.length > 4096) throw new Error('Input exceeds 4 KiB.');
      const transforms = {
        base64: () => to64(data), unbase64: () => decoder.decode(from64(text)),
        hex: () => toHex(data), unhex: () => {
          if (!/^(?:[0-9a-f]{2})*$/i.test(text)) throw new Error('Enter pairs of hexadecimal digits.');
          return decoder.decode(Uint8Array.from(text.match(/../g) || [], pair => parseInt(pair, 16)));
        },
        url: () => encodeURIComponent(text), unurl: () => decodeURIComponent(text),
        sha: async () => {
          if (!crypto.subtle) throw new Error('Open Toolbox on localhost to calculate a digest.');
          return toHex(new Uint8Array(await crypto.subtle.digest('SHA-256', data)));
        }
      };
      result.textContent = await transforms[operation.value]();
      feedback.textContent = 'Transform complete.';
    }), feedback);
  }

  function usbStudio(macro) {
    if (!navigator.serial) {
      panel('Serial access requires a supported browser', 'Open PocketOS on localhost in a browser with Web Serial. Pair your own board through its COM/UART USB connector.');
      return;
    }
    const element = panel(macro ? 'Paired media shortcuts' : 'Board serial monitor',
      'Close any other serial monitor first. Choose the board’s serial port. Native media shortcuts require the Native USB build and native USB port.');
    const controls = node('div', '', 'actions');
    element.append(controls);
    const feedback = status(element);
    const readings = output(element);
    let port;
    let reader;
    let readTask;
    let armedUntil = 0;
    let abandoned = false;
    async function closePort() {
      armedUntil = 0;
      const active = port;
      port = undefined;
      if (!active) return;
      await reader?.cancel().catch(() => {});
      await readTask?.catch(() => {});
      await active.close().catch(() => {});
    }
    controls.append(action('Choose serial port', feedback, async () => {
      if (port) throw new Error('Disconnect the current port first.');
      const picked = await navigator.serial.requestPort();
      if (abandoned) return;
      await picked.open({baudRate:115200});
      if (abandoned) { await picked.close(); return; }
      port = picked;
      const stream = new TextDecoder();
      reader = port.readable.getReader();
      readTask = (async () => {
        try {
          while (port === picked) {
            const {done, value} = await reader.read();
            if (done) break;
            readings.textContent = (readings.textContent + stream.decode(value, {stream:true})).slice(-4096);
          }
        } catch (error) { feedback.textContent = `Serial read ended: ${error.message}`; }
        finally { reader.releaseLock(); reader = undefined; }
      })();
      feedback.textContent = 'Serial port open at 115200 baud.';
    }), action('Disconnect', feedback, async () => {
      await closePort();
      feedback.textContent = 'Serial port closed.';
    }));
    async function write(command) {
      if (!port?.writable) throw new Error('Pair the board first.');
      const writer = port.writable.getWriter();
      try { await writer.write(encoder.encode(command)); }
      finally { writer.releaseLock(); }
    }
    controls.append(action('Read board status', feedback, () => write('?')));
    if (macro) {
      async function send(command) {
        if (!port?.writable || performance.now() > armedUntil) throw new Error('Pair and arm the serial controls first.');
        armedUntil = 0;
        await write(command);
        feedback.textContent = 'Command sent. Check the serial reply for confirmation.';
      }
      controls.append(action('Arm one command', feedback, async () => {
        await write('U');
        armedUntil = performance.now() + 15000;
        feedback.textContent = 'Arm request sent. Check the serial reply before choosing an action.';
      }), action('Play / pause', feedback, () => send('P')),
      action('Next track', feedback, () => send('N')),
      action('Previous track', feedback, () => send('B')),
      action('Disarm', feedback, async () => { armedUntil = 0; await write('X'); feedback.textContent = 'Disarm request sent.'; }));
    }
    dispose = () => { abandoned = true; closePort().catch(() => {}); };
  }

  function soundforge() {
    const element = panel('Eight-step tone sequencer', 'Choose notes for each step, set the tempo, and play through this browser.');
    const [tempoLabel, tempo] = field('Tempo (BPM)');
    tempo.type = 'number';
    tempo.min = '40';
    tempo.max = '220';
    tempo.value = '100';
    const grid = node('div', '', 'app-grid');
    const tones = [['Rest',0],['C4',261.63],['D4',293.66],['E4',329.63],['G4',392],['A4',440]];
    const steps = Array.from({length:8}, (_, index) => {
      const [label, select] = field(`Step ${index + 1}`, 'select');
      for (const [name, frequency] of tones) {
        const item = node('option', name);
        item.value = frequency;
        select.append(item);
      }
      select.selectedIndex = [1,0,3,0,4,0,3,0][index];
      grid.append(label);
      return select;
    });
    const controls = node('div', '', 'actions');
    element.append(tempoLabel, grid, controls);
    const feedback = status(element);
    let context;
    let timer;
    let tone;
    let step = 0;
    const midiNotes = [0, 60, 62, 64, 67, 69];
    function getBpm() {
      const bpm = Number(tempo.value);
      if (bpm < 40 || bpm > 220 || !Number.isFinite(bpm)) throw new Error('Choose a tempo from 40–220 BPM.');
      return bpm;
    }
    function midiFile() {
      const micros = Math.round(60000000 / getBpm());
      const track = [0, 255, 81, 3, micros >> 16, (micros >> 8) & 255, micros & 255];
      const delta = ticks => {
        const encoded = [ticks & 127];
        while ((ticks >>= 7) > 0) encoded.unshift((ticks & 127) | 128);
        return encoded;
      };
      let delay = 0;
      for (const step of steps) {
        const note = midiNotes[step.selectedIndex];
        if (note) {
          track.push(...delta(delay), 0x90, note, 80, ...delta(120), 0x80, note, 0);
          delay = 360;
        } else delay += 480;
      }
      track.push(...delta(delay), 255, 47, 0);
      const length = track.length;
      return Uint8Array.from([
        77,84,104,100,0,0,0,6,0,0,0,1,1,224,77,84,114,107,
        length >>> 24, (length >>> 16) & 255, (length >>> 8) & 255, length & 255, ...track
      ]);
    }
    function stop() {
      clearInterval(timer);
      timer = undefined;
      if (tone) { try { tone.stop(); } catch {} tone = undefined; }
    }
    controls.append(action('Play sequence', feedback, async () => {
      const bpm = getBpm();
      stop();
      if (!context) context = new AudioContext();
      await context.resume();
      step = 0;
      const beat = () => {
        const hz = Number(steps[step++ % steps.length].value);
        if (!hz) return;
        const oscillator = context.createOscillator();
        const gain = context.createGain();
        const now = context.currentTime;
        oscillator.frequency.value = hz;
        gain.gain.setValueAtTime(0.09, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + 0.14);
        oscillator.connect(gain).connect(context.destination);
        oscillator.onended = () => { oscillator.disconnect(); gain.disconnect(); };
        oscillator.start(now);
        oscillator.stop(now + 0.15);
        tone = oscillator;
      };
      beat();
      timer = setInterval(beat, 60000 / bpm);
      feedback.textContent = 'Playing through this browser.';
    }), button('Stop playback', () => { stop(); feedback.textContent = 'Playback stopped.'; }),
    action('Download MIDI', feedback, async () => {
      const url = URL.createObjectURL(new Blob([midiFile()], {type:'audio/midi'}));
      const link = node('a');
      link.href = url;
      link.download = 'soundforge.mid';
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      feedback.textContent = 'MIDI sequence exported.';
    }));
    dispose = () => { stop(); context?.close().catch(() => {}); };
  }

  function portalplay() {
    const element = panel('Memory tiles', 'Reveal two tiles at a time and match all eight pairs.');
    const grid = node('div', '', 'app-grid game-grid');
    element.append(grid);
    const feedback = status(element);
    let pending;
    function play() {
      clearTimeout(pending);
      grid.replaceChildren();
      const deck = [...'ABCDEFGHABCDEFGH'];
      for (let i = deck.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [deck[i], deck[j]] = [deck[j], deck[i]];
      }
      let first;
      let pairs = 0;
      let moves = 0;
      let busy = false;
      feedback.textContent = '0 pairs found.';
      deck.forEach((mark, index) => {
        const tile = button('?', () => {
          if (busy || first?.tile === tile || tile.disabled) return;
          tile.textContent = mark;
          tile.setAttribute('aria-label', `Tile ${index + 1}, ${mark}`);
          if (!first) { first = {tile, mark, index}; return; }
          moves++;
          if (first.mark === mark) {
            first.tile.disabled = tile.disabled = true;
            first = undefined;
            pairs++;
            feedback.textContent = pairs === 8 ? `Complete in ${moves} moves.` : `${pairs} of 8 pairs found.`;
          } else {
            busy = true;
            pending = setTimeout(() => {
              first.tile.textContent = tile.textContent = '?';
              first.tile.setAttribute('aria-label', `Tile ${first.index + 1}, hidden`);
              tile.setAttribute('aria-label', `Tile ${index + 1}, hidden`);
              first = undefined;
              busy = false;
            }, 650);
          }
        });
        tile.setAttribute('aria-label', `Tile ${index + 1}, hidden`);
        grid.append(tile);
      });
    }
    element.insertBefore(button('New game', play), grid);
    play();
    dispose = () => clearTimeout(pending);
  }

  function cyberarena() {
    const element = panel('Defensive practice', 'Solve three offline exercises using fixed practice data.');
    const exercises = [
      ['Decode the Base64 text TkVYVVM=.', ['NEXUS', 'NEXOS', 'N3XUS'], 0],
      ['A downloaded file has a different SHA-256 digest from its signed manifest. What should you do?', ['Run it offline', 'Quarantine it and investigate', 'Rename it'], 1],
      ['Which permission best fits a service that only reads a diagnostic log?', ['Administrator', 'Read and write the folder', 'Read the required log'], 2]
    ];
    const question = node('h3');
    const choices = node('div', '', 'actions');
    element.append(question, choices);
    const feedback = status(element);
    let index = 0;
    function next() {
      choices.replaceChildren();
      if (index === exercises.length) {
        question.textContent = 'All three exercises complete.';
        choices.append(button('Play again', () => { index = 0; next(); }));
        return;
      }
      const [prompt, options, answer] = exercises[index];
      question.textContent = prompt;
      options.forEach((option, optionIndex) => {
        choices.append(button(option, () => {
          if (optionIndex !== answer) { feedback.textContent = 'Try another answer.'; return; }
          feedback.textContent = 'Correct.';
          index++;
          next();
        }));
      });
    }
    next();
  }

  const handlers = {airscope:() => radio('wifi'), bluescope:() => loopback ? bluetoothReader() : radio('ble'), ghostsense:() => radio('csi'),
    cyberdeck, deaddrop, pocketvault, toolbox, 'usb-studio':() => usbStudio(false),
    ghostlink:() => usbStudio(true), soundforge, portalplay, cyberarena};
  function close() {
    dispose();
    dispose = () => {};
    body.replaceChildren();
  }
  window.pocketApps = {
    catalog,
    close,
    open(app) {
      close();
      byId('application-name').textContent = app.name;
      byId('application-detail').textContent = app.detail;
      handlers[app.id]();
    }
  };
  if (loopback) {
    byId('connection').textContent = 'Local preview';
    renderCatalog(catalog.map(app => localApps.has(app.id) ? app : {...app, state:'device required'}));
  }
  showView();
})();
