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

Next, DEV-1B adds the single-owner service manager and Guard admission, stop acknowledgements, per-service budgets, largest-block checks and thermal policy. The first device-facing service will be the management AP. BLE lifecycle remains gated on callback ownership and repeatability.
