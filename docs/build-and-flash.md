# Build and host tests

With Arduino IDE's bundled Arduino CLI 1.6.0 and ESP32 core 3.3.12 already installed, run this from the workspace in PowerShell:

```powershell
& ./scripts/build.ps1
```

The script accepts `-ArduinoCli` and `-ArduinoData` for existing installations at other paths. It uses the historical board menu options, creates a distinct `build/esp32s3-*` directory, and captures the compile output. Successful verification checks the partition image hash, 3 MiB application slot limit and linked startup symbols, then writes a manifest with binary, ELF, map and partition hashes. The launcher may need a regular Windows process if the sandbox rejects Xtensa executables with `Illegal System DLL Relocation`.

The first verified build used Xtensa GCC 14.2.0 and produced a 247,888-byte application image. Arduino reported 247,734 bytes of program storage and 21,484 bytes of static dynamic-memory use. The esptool image metadata identifies the bundled Arduino library build. Use the product version printed at boot and the matching ELF hash to identify the specific NEXUS-S3 build.

Run the pure boot-policy tests from a Visual Studio 2022 x64 Developer Command Prompt at the workspace root:

```cmd
if not exist build\host mkdir build\host
cl /nologo /std:c++17 /EHsc /W4 /WX /Fo:build\host\boot_policy_test.obj /Fe:build\host\boot_policy_test.exe tests\host\boot_policy_test.cpp
build\host\boot_policy_test.exe
cl /nologo /std:c++17 /EHsc /W4 /WX /Fo:build\host\ /Fe:build\host\guard_test.exe tests\host\guard_test.cpp firmware\NexusS3\guard.cpp
build\host\guard_test.exe
```

Run the desktop and mobile browser fixture with Node.js 24 and local Microsoft Edge:

```powershell
node tests/host/browser_test.mjs
```

The browser runner binds loopback and uses a separate temporary browser profile under `build/host`. It serves the production frontend with explicitly substituted API data and exercises browser behavior. ESP32 protocol and radio acceptance require supervised device testing.

## Physical acceptance gate

Build artifacts are for local verification. A proposed device operation requires explicit approval of the exact artifact and command. Before any upload, identify the active app slot, confirm live partition compatibility and current backup status, and review the exact write regions and recovery path. The ordinary Arduino upload recipe can write bootloader, partition and OTA metadata. Keep those regions out of any proposed app-only operation unless deliberately reviewed and approved. Use the board's UART connector and the documented cooling setup for the first physical acceptance.

After an approved upload, monitor UART at 115200. Confirm the boot build ID and hardware readings, send `A` to request management, then connect one client to the SSID with the displayed password and open the displayed URL. Enter the one-time operator code in Settings when ready to test controls. Apply a refresh preference, cancel a queued stop within the displayed window, and check for `MANAGEMENT STOP CANCELLED` on UART. Then request a second stop and check for both `MANAGEMENT STOP SOURCE BROWSER` and `MANAGEMENT STOP CONFIRMED`. Use `S` for a direct UART stop. Capture logs with the provisioning secrets removed. Stop immediately on an integrity fault, unexpected reset, thermal cutoff or unconfirmed release.
