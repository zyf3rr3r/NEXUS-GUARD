# Qualification register

Reconciled 2026-11-26. Historical entries describe the diagnostic firmware and its specific workload. Foundation entries describe `0.1.0-dev.1` and the pinned DEV-1A build profile.

| ID | Requirement and method | Result | Evidence and next action |
|---|---|---|---|
| DIAG-23 | v2.3 concurrent Wi-Fi, BLE, CPU, PSRAM and FFat, followed by recovery | PASS | Original serial transcript and handoff's client summary. Preserve the qualification conditions. |
| HIST-LAYOUT | Compare historical board partition readback, v2.3 cached build and installed profile | PASS | All six entries and flags agree. Confirm live compatibility before an approved upload. |
| HIST-BACKUP | Compute local SHA-256 of both backups | PASS | Full flash backup matches its sidecar. Original SPIFFS backup matches the continuity handoff. |
| HIST-BLE | Preserve matching Phase 10 crash evidence | PASS | ELF hash matches the continuity handoff. Decoded trace is present. |
| BOOT-HOST | Exercise the pure boot policy with MSVC x64 in C++17 mode | PASS | 10/10 cases, including exact reserves, each individual boot fault and simultaneous failures. Evidence: `build/host/boot_policy_test.log`. |
| BLD-01 | Compile firmware, check linked startup, confirm partition image and app slot fit | PASS | `build/esp32s3-20261125-183003-9155076/manifest.json` and `compile.log`. The 247,888-byte app fits the 3,145,728-byte slot. The partition image matches v2.3 byte for byte, and esptool image validation passed. |
| HAL-01 | Compare real serial readings with the board and approved build | NOT_RUN | Pending supervised device acceptance. |
| GRD-01 through GRD-04 | Admission, thermal and memory budgets, lifecycle, faults and cancellation | DEFERRED | DEV-1B. Add deterministic tests before adding radio drivers. |
| NET-01 through NET-03, UI-01, CFG-01 | Management AP, HTTP bounds/authentication, offline UI and settings | DEFERRED | DEV-1C/D, then supervised device acceptance. |
| STR-01 | Preserve existing FFat data and degrade safely on mount failure | DEFERRED | FFat stays idle in the first checkpoint. Qualify the storage adapter using approved test data. |
| BLE-01 / BLE-02 | Cold-start repeatability, same-boot transitions and teardown synchronization | DEFERRED | Earlier corrected runs used a settling delay. Required before BlueScope is ready. |
| MEM-01 / THM-01 | Stabilized service memory retention and thermal endurance | DEFERRED | Test after services exist, with documented ambient and airflow. Passive cooling is unqualified. |
| USB-01 | Integrated CDC/bulk with PSRAM, HID and MIDI acceptance | DEFERRED | Historical standalone CDC/bulk evidence remains separate. Check actual GPIO and board variant before new peripheral use. |
| SEC-01 | Production auth, encryption, updates and security readiness | DEFERRED | Qualify according to the product modules and release criteria. |
| REC-01 | Backup restoration | NOT_RUN | Requires a separately authorized recovery procedure. |
| CSI-01 | Authorized sensing feasibility and privacy | DEFERRED | Dedicated research milestone. |
