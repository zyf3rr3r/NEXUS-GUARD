# Architecture and framework decision

## DEV-0 decision

Use Arduino CLI 1.6.0 and Arduino ESP32 3.3.12 as the initial build framework. Pin the cached v2.3 ESP32-S3 profile, including the bundled ESP-IDF 5.5.5, Xtensa tool package 2601 and esptool 5.3.1. The workspace audit found an empty product directory with no Git metadata. Historical sketches and diagnostic artifacts are located outside this directory and stay in place.

Native ESP-IDF offers finer startup and driver ownership. Adopting it now would add a standalone SDK and requalification of the board, bootloader, storage, radio and USB profile. Arduino as an ESP-IDF component would also require validating exact version compatibility. Revisit native ESP-IDF when service lifecycle or SDK configuration requires it. Keep the pure policy independent of either SDK and isolate device calls in the HAL to make that migration practical.

Retain FFat for future user storage under the qualified partition layout. Mount only through a future storage owner, with formatting disabled. NVS configuration needs explicit initialization, schema and error handling before it is enabled.

## DEV-1A implementation

The firmware retains the pinned Arduino task and CPU profile. The linker routes Arduino's `initArduino()` call to an empty product hook because the default hook erases the NVS partition after certain initialization failures. The build checks the linked ELF for the replacement and rejects a linked default hook. Evaluate dependencies on the skipped initialization before importing higher-level Arduino services.

The firmware reads back the configured 160 MHz clock, checks heap integrity, then reads the physical flash capacity, detected PSRAM, on-chip temperature and internal/PSRAM heap metrics. It checks the six flash partition entries, and prints the ELF build ID, SDK version, chip revision, core count and reset reason over the UART console. The boot policy latches a serial safe state for a failing initial check. A healthy boot prints snapshots every five seconds. Heap minima are SDK capability-heap low-water metrics and temperature is an on-chip reading. No CPU utilization is inferred.

| Boot flag | Meaning |
|---:|---|
| `0x01` | CPU clock differs from the qualified profile |
| `0x02` | Physical flash size unavailable or unexpected |
| `0x04` | PSRAM missing or unexpected |
| `0x08` | Partition layout mismatch |
| `0x10` | Startup heap integrity failed |
| `0x20` | On-chip temperature unavailable or outside the selected 10–50°C sensor range |
| `0x40` | Internal free heap below 64 KiB |
| `0x80` | PSRAM free below 1 MiB |

## DEV-1B through DEV-1D implementation

The Arduino loop owns the management service. It starts only from UART after boot capability checks and fresh Guard admission. Wi-Fi and HTTP are one lease because the HTTP transport depends on the AP. The SDK event loop and network infrastructure stay initialized after the first session. The AP interface, Wi-Fi driver and HTTP server have per-session ownership. Confirmed shutdown stops HTTP, waits for the SDK AP stop event, releases the AP interface and driver, invalidates the operator session, and checks heap integrity. If shutdown cannot confirm release, Guard retains the lease and enters a latched fault. A corrupted heap calls for controlled power-off and local preservation of the UART log.

Guard requires an on-chip temperature of 33.3°C or lower to admit a session. The cutoff is ten degrees above the admitted reading and stays fixed for that session. The warning begins 2°C below cutoff and clears below 3°C of margin. Admission reserves 64 KiB of internal heap plus 72 KiB of management headroom, requires a 32 KiB contiguous internal block, and preserves 1 MiB of PSRAM. Runtime supervision samples once per second and stops on invalid temperature, cutoff, reserve breach, or ten-minute expiry. Confirmed stop starts a 30-second cooldown. These are conservative starting values for supervised product testing. The stop-event wait is bounded to 1.5 seconds. SDK calls themselves still need on-device timing and fault qualification.

The HTTP task reads synchronized copies of a single cached telemetry sample and submits stop jobs to a one-slot queue. A browser stop has a 15-second cancellation window that starts after the acceptance response is sent. An unarmed job is discarded if sending the response fails. PocketOS displays the remaining time supplied by the server and leaves the window free of stop-job polling. Guard limits and direct UART stops still take effect immediately. UART identifies the stop source, logs accepted cancellations and confirms release after the network goes offline. The redacted event ring holds eight records in RAM. The UI is framework-free HTML, CSS and JavaScript packaged into the application image at build time. It uses local assets, bounded history and reconnect backoff.

Each boot generates an AP passphrase before the RF and ADC stacks start. Each management session gets a separately generated, one-time operator code. Both are provisioned over UART. The AP uses WPA2 on channel 1 with one client and 8.5 dBm verified after startup. Management settings apply for the current boot. The optional browser theme persists in browser storage. The AP uses RAM-only Wi-Fi storage, and product startup leaves NVS, FFat, BLE, HID, and MIDI idle. BLE lifecycle remains gated on callback ownership and repeatability.

The pinned SDK logs a PHY calibration-cache error on Wi-Fi startup when NVS is unopened. ESP-IDF falls back to full RF calibration in that case. The device test reached a running AP after this message. [ESP-IDF PHY initialization](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_phy/src/phy_init.c#L876-L924)

### PocketOS interface design

The default dark theme gives PocketOS a green security-console look with near-black surfaces, thin green-tinted borders and bright green for primary actions, current navigation and a live connection. Warm orange marks warnings and the stop control. System sans-serif keeps headings and labels readable, while monospace distinguishes key readings, connection state and build IDs. The header offers a light green and white theme, and the browser remembers the selection when local storage is available.

Overview, ChipVitals and Settings share a desktop sidebar. At widths of 800 px and below the sidebar becomes a horizontal navigation row beneath the header. At widths of 600 px and below paired panels stack. Keyboard focus has a visible outline, and moving between views places focus on the destination heading. The interface serves its stylesheet and script from the firmware image and uses system fonts. Browser fixtures exercise interface states with simulated data. On-device UI qualification is pending in the [qualification register](qualification-register.md).
