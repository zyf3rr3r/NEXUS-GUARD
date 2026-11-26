# App suite test candidate

The validated foundation is `0.1.0-dev.3`. The current management candidate is `0.2.0-dev.3`, which adds explicit scan admission/result codes and adjusts the optional-radio internal heap floor to `128 KiB` for Wi-Fi AP + NimBLE coexistence. Device tests should use the documented, approved app-only upload procedure and preserve the tested foundation image and its recovery files.

## Build and preview

Run `./scripts/build.ps1` for the existing UART and management profile. Use `./scripts/build.ps1 -NativeUsb` for the separate TinyUSB profile. Both must pass the original partition hash check. The native profile enumerates CDC and media HID. Media reports require an explicit arm request over the paired native serial port followed by one allowed action. The arm expires after 15 seconds and is cleared when CDC disconnects. Media controls are available while management is stopped.

The secure localhost preview has no device API. Start it with Node.js from the workspace root and open [PocketOS localhost preview](http://127.0.0.1:8077/?offline=1).

```powershell
node scripts/serve-local.mjs
```

Use the board's AP for hardware radio, inbox and diagnostic routes. Use localhost for browser APIs that require a secure context, including Web Crypto, Web Serial and Web Bluetooth. Use a browser with the applicable API and permissions. The preview and firmware package the same client files.

## Individual acceptance order

| App | First implementation | Device or operator acceptance |
|---|---|---|
| AirScope | One bounded passive 2.4 GHz scan. Up to 12 records show advertised names, channels, RSSI and security modes. | While the board is cool, send `A` over UART and wait for `MANAGEMENT START NONE`. Send `W` once and look for `RADIO WIFI DONE count=…`. Then unlock Settings, open AirScope and choose **Load last scan**. Check a known AP appears and the management link is usable. Test the browser Scan and cancellation separately. |
| BlueScope | Short passive ESP32 BLE discovery. Localhost also supports browser-selected GATT characteristic reads by UUID. | Scan an owned advertising peripheral, then cancel a second scan. Verify that active discovery ends before Wi-Fi shutdown. Test an explicitly selected readable GATT characteristic via localhost. |
| CyberDeck | Local task checks for the serving board's identity, telemetry, Guard and AP policy. | Compare results with the management API and UART, then confirm failures are displayed if the board goes offline. |
| DeadDrop | An authenticated board inbox holds eight messages, each up to 96 printable ASCII bytes. New entries replace the oldest. Retention lasts for the current boot. | Test an unauthorized read, then send a note with operator controls unlocked. Start another authorized session during the same boot and verify it can read the note. |
| PocketVault | Local encrypted note envelopes using Web Crypto AES-GCM and PBKDF2. A browser can save an authenticated envelope. | On localhost, encrypt and reopen one note, reject a wrong passphrase and edited envelope, and confirm the saved object is ciphertext. |
| Toolbox | UTF-8 Base64, hex and URL transforms. SHA-256 is available on a secure browser origin. | Run known vectors and invalid inputs on the board page and the localhost preview. |
| USB Studio | Explicit Web Serial port selection, bounded terminal view and a board status command. | On localhost, open one owned port. Verify the default profile via COM/UART and the separate native profile via native USB in separate sessions. |
| GhostLink | Native-profile USB media controls for play/pause, next and previous, with one-action arming and release. | Use one native USB cable and an owned host with a harmless media app. Check enumeration, denial before arming, one action after arming, expiry, disarm and release. |
| SoundForge | Browser-local eight-step oscillator sequencer with a standard MIDI file export. | Play, stop, switch pages during playback and import the exported file into a MIDI-capable app. |
| PortalPlay | Offline randomized 16-tile memory game. | Complete a board with keyboard and touch. Check restart and mismatched tiles. |
| GhostSense | Three-second CSI experiment. Collect at most 192 aggregate samples attributed to the single AP client and compare mean amplitude with a baseline. | Capture an owned link. Confirm eligible frames, compare repeated baselines and check shutdown. A zero-frame result means the experiment did not obtain a usable sample. |
| CyberArena | Three offline exercises using fixed data about encoding, integrity checks and least privilege. | Verify answers, progression, retry and restart in the local app. |

Radio tools use one shared job slot and stop on insufficient Guard margin, heap reserves, a cancellation or the time budget. The Wi-Fi adapter briefly enters AP-plus-station mode for discovery without joining a network. BLE discovery leaves its initialized host resident and limits active scanning. Characterize resident memory and coexistence on the board. CSI exposes aggregate amplitude and sample counts. Evaluate it as a signal experiment before adding any presence inference.

The first AirScope trial showed a cancelled scan followed by a busy-or-recovering response. The revised UI reports thermal margin, memory margin, driver availability, active jobs and cooldown separately. An admission denial skips the cooldown. UART `W` lets the operator queue a scan early in a cool management session while browser login is still pending. Browser retrieval of the result still requires an unlocked operator session. The three-degree margin to the Guard cutoff remains in force.

The inbox uses volatile memory. The vault stores browser-local encrypted envelopes. These first versions leave the qualified FFat and NVS data paths out of app persistence. Board-hosted APIs use the foundation's existing local management authority.
