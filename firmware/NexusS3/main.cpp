#include "boot_policy.h"
#include "field_policy.h"
#include "field_store.h"
#include "hal.h"
#include "management.h"
#include "native_usb.h"
#include "radio_policy.h"
#include "status_led.h"

#include <cinttypes>
#include <cstdio>
#include "Arduino.h"
#include "HardwareSerial.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || F_CPU != 160000000L
#error "Build NEXUS-S3 for the qualified ESP32-S3 160 MHz profile."
#endif

namespace {

constexpr char kVersion[] = "0.3.0-dev.1";
constexpr uint8_t kBootButtonPin = 0;
constexpr size_t kCommandBufSize = 80;

nexus::FieldStore fieldStore;
nexus::FieldButton fieldButton;
nexus::StatusLed statusLed;
nexus::ManagementNetwork network;
nexus::ManagementService service(network);
char buildId[65]{};
unsigned cores = 0;
int resetReason = 0;
bool telemetryEnabled = false;
uint64_t nextSample = 0;
uint64_t nextSerial = 0;
uint32_t sequence = 0;
const char* optionalAppsBlock = "RADIO_MANAGEMENT_REQUIRED";

char cmdBuf[kCommandBufSize]{};
size_t cmdLen = 0;
uint64_t cmdStartedMs = 0;

void printMemory(const char* name, const nexus::MemorySample& memory) {
  std::printf(" %s_free=%" PRIu32 " %s_largest=%" PRIu32 " %s_min=%" PRIu32,
              name, memory.freeBytes, name, memory.largestBlockBytes, name, memory.minimumFreeBytes);
}

void printTelemetry(const nexus::Telemetry& sample) {
  std::printf("STATUS uptime_ms=%" PRIu64 " cpu_mhz=%" PRIu32 " on_chip_c=",
              sample.uptimeMs, sample.cpuMHz);
  if (sample.temperatureValid) {
    std::printf("%.2f", static_cast<double>(sample.temperatureC));
  } else {
    std::printf("unavailable");
  }
  printMemory("internal", sample.internal);
  printMemory("psram", sample.psram);
  std::putchar('\n');
}

void printFieldStatus() {
  std::printf("FIELD_MODE nvs=%s wifi=%s pass=%s standalone=%s\n",
              fieldStore.nvsReady() ? "ok" : "unavailable",
              fieldStore.hasWifi() ? "configured" : "ephemeral",
              fieldStore.hasPass() ? "configured" : "ephemeral",
              fieldStore.active() ? "ready" : "off");
}

uint64_t nowMs() { return esp_timer_get_time() / 1000; }

nexus::GuardSample guardSample(const nexus::Telemetry& sample) {
  return {sample.uptimeMs, sample.temperatureC, sample.temperatureValid,
          sample.internal.freeBytes, sample.internal.largestBlockBytes, sample.psram.freeBytes};
}

void publish(const nexus::Telemetry& sample) {
  network.publish({sample, ++sequence, service.state(), service.lastReason(), service.warning(),
                   service.guard().cutoff(), service.guard().leased()});
}

bool checkHeap() {
  if (heap_caps_check_integrity_all(true)) return true;
  esp_rom_printf("SAFE_MODE HEAP_INTEGRITY. Power off and preserve this log.\n");
  service.latchIntegrityFault();
  telemetryEnabled = false;
  return false;
}

void clearCommandBuffer() {
  mbedtls_platform_zeroize(cmdBuf, sizeof(cmdBuf));
  cmdLen = 0;
  cmdStartedMs = 0;
}

void handleFieldLine(uint64_t now) {
  const auto parsed = nexus::parseFieldCommand(cmdBuf, cmdLen);
  if (parsed.kind == nexus::FieldCommand::Kind::Status) {
    printFieldStatus();
  } else if (parsed.kind == nexus::FieldCommand::Kind::Invalid) {
    std::puts("FIELD_ERROR INVALID_COMMAND");
  } else if (service.state() != nexus::ServiceState::Stopped) {
    std::puts("FIELD_ERROR STOP_MANAGEMENT_FIRST");
  } else if (parsed.kind == nexus::FieldCommand::Kind::SetWifi) {
    if (fieldStore.saveWifiPassword(parsed.arg, parsed.argLen)) {
      statusLed.notifySaved(now);
      std::puts("FIELD_WIFI SAVED");
      printFieldStatus();
    } else {
      statusLed.notifyDenied(now);
      std::puts("FIELD_ERROR WIFI_SAVE_FAILED");
    }
  } else if (parsed.kind == nexus::FieldCommand::Kind::SetPass) {
    if (fieldStore.saveOperatorPassphrase(parsed.arg, parsed.argLen)) {
      statusLed.notifySaved(now);
      std::puts("FIELD_PASS SAVED");
      printFieldStatus();
    } else {
      statusLed.notifyDenied(now);
      std::puts("FIELD_ERROR PASS_SAVE_FAILED");
    }
  } else if (parsed.kind == nexus::FieldCommand::Kind::Clear) {
    if (fieldStore.clear()) {
      statusLed.notifySaved(now);
      std::puts("FIELD_MODE CLEARED");
      printFieldStatus();
    } else {
      statusLed.notifyDenied(now);
      std::puts("FIELD_ERROR CLEAR_FAILED");
    }
  }
  clearCommandBuffer();
}

int readSerialCommand(uint64_t now) {
  if (cmdLen > 0 && now >= cmdStartedMs && now - cmdStartedMs > 5000) {
    clearCommandBuffer();
    std::puts("FIELD_ERROR COMMAND_TIMEOUT");
  }
  while (Serial0.available() > 0) {
    const int ch = Serial0.read();
    if (ch < 0) break;
    if (cmdLen == 0) {
      if (ch == ':') {
        cmdBuf[cmdLen++] = ':';
        cmdStartedMs = now;
        continue;
      }
      if (ch == '\r' || ch == '\n') continue;
      return ch;
    }
    if (ch == '\r' || ch == '\n') {
      handleFieldLine(now);
      continue;
    }
    if (cmdLen + 1 >= sizeof(cmdBuf)) {
      clearCommandBuffer();
      std::puts("FIELD_ERROR COMMAND_TOO_LONG");
      continue;
    }
    cmdBuf[cmdLen++] = static_cast<char>(ch);
  }
  return -1;
}

}  // namespace

const char* nexusVersion() { return kVersion; }
const char* nexusBuildId() { return buildId; }
unsigned nexusCoreCount() { return cores; }
int nexusResetReason() { return resetReason; }

// The pinned build redirects Arduino's startup hook here to avoid auto-erasing NVS.
extern "C" void __wrap_initArduino() {}

void setup() {
  if (!checkHeap()) return;

  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  cores = chip.cores;
  resetReason = static_cast<int>(esp_reset_reason());
  esp_app_get_elf_sha256(buildId, sizeof(buildId));
  std::printf("NEXUS-S3 %s build=%s sdk=%s reset=%d policy=guard-v1\n",
              kVersion, buildId, esp_get_idf_version(), resetReason);

  fieldStore.init();
  network.attachFieldStore(&fieldStore);
  network.preparePassword();
  nexus::initTemperatureSensor();
  pinMode(kBootButtonPin, INPUT_PULLUP);
  statusLed.init();

  const auto initial = nexus::sampleTelemetry();
  const auto faults = nexus::bootFaults({
      initial.cpuMHz, initial.flashBytes, initial.psramBytes, nexus::checkPartitionLayout(),
      true, initial.temperatureValid, initial.internal.freeBytes, initial.psram.freeBytes,
  });
  std::printf("BOOT chip=ESP32-S3 revision=%u cores=%u flash=%" PRIu32 " psram=%" PRIu32
              " mode=%s faults=0x%02" PRIX32 "\n",
              static_cast<unsigned>(chip.revision), static_cast<unsigned>(chip.cores),
              initial.flashBytes, initial.psramBytes, faults == 0 ? "IDLE" : "SAFE_MODE", faults);
  printFieldStatus();
  printTelemetry(initial);
  if (faults != 0) {
    statusLed.update(nexus::ServiceState::Faulted, nexus::Reason::None, false, nowMs());
    std::puts("Startup checks failed. Preserve this log and compare the build profile and boot fault flags.");
    return;
  }
  Serial0.begin(115200);
  nexus::setupNativeUsb();
  publish(initial);
  telemetryEnabled = true;
  std::puts("UART: A start, S stop, K rotate code, W AirScope, ? status, :field/:wifi/:pass/:clear. BOOT btn: hold 1.5s start, press stop.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!telemetryEnabled) return;
  const auto now = nowMs();
  const auto before = service.state();
  const bool running = before == nexus::ServiceState::Running;
  nexus::pollNativeUsb(now, before == nexus::ServiceState::Stopped);

  const auto buttonAction = fieldButton.poll(digitalRead(kBootButtonPin) == LOW, now, running);
  const int command = readSerialCommand(now);
  const bool wantStart = command == 'A' || buttonAction == nexus::FieldButton::Action::Start;
  const bool buttonStop = buttonAction == nexus::FieldButton::Action::Stop;
  const bool uartStop = command == 'S';
  const bool browserStop = !wantStart && !uartStop && !buttonStop && network.takeStop(now);

  if (wantStart) {
    if (before == nexus::ServiceState::Stopped && !checkHeap()) return;
    if (buttonAction == nexus::FieldButton::Action::Start) std::puts("MANAGEMENT START SOURCE BUTTON");
    const auto sample = nexus::sampleTelemetry();
    const auto reason = service.start(guardSample(sample));
    std::printf("MANAGEMENT START %s\n", nexus::reasonCode(reason));
    if (reason != nexus::Reason::None) statusLed.notifyDenied(now);
    nextSample = 0;
  } else if (uartStop || buttonStop || browserStop) {
    const char* source = uartStop ? "UART" : buttonStop ? "BUTTON" : "BROWSER";
    std::printf("MANAGEMENT STOP SOURCE %s\n", source);
    std::printf("MANAGEMENT STOP %s\n", nexus::reasonCode(service.stop(now, nexus::Reason::StopRequested)));
    nextSample = 0;
  } else if (command == 'K' && running) {
    network.rotateOperatorCode();
  } else if (command == 'W') {
    const auto* reason = network.queueRadio(nexus::RadioApps::Kind::Wifi, now);
    std::printf("RADIO WIFI UART %s\n", reason ? reason : "QUEUED");
  } else if (command == '?') {
    printTelemetry(nexus::sampleTelemetry());
  }

  statusLed.update(service.state(), service.warning(),
                   fieldButton.holdingToStart(service.state() == nexus::ServiceState::Running), now);

  if (now < nextSample) {
    network.tickApps(now, optionalAppsBlock);
    return;
  }
  auto sample = nexus::sampleTelemetry();
  service.tick(guardSample(sample));
  if (before == nexus::ServiceState::Running && service.state() != before) {
    std::printf("GUARD %s\n", nexus::reasonCode(service.lastReason()));
    if (service.state() == nexus::ServiceState::Stopped && !checkHeap()) return;
    sample = nexus::sampleTelemetry();
  }
  publish(sample);
  optionalAppsBlock = nexus::optionalRadioBlock(service.state(), service.warning(), service.guard().cutoff(), sample);
  network.tickApps(sample.uptimeMs, optionalAppsBlock);
  statusLed.update(service.state(), service.warning(),
                   fieldButton.holdingToStart(service.state() == nexus::ServiceState::Running), now);
  nextSample = sample.uptimeMs + 1000;
  if (now >= nextSerial) {
    printTelemetry(sample);
    nextSerial = now + 5000;
  }
}
