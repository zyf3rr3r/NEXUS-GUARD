#include "boot_policy.h"
#include "hal.h"
#include "management.h"

#include <cinttypes>
#include <cstdio>
#include "HardwareSerial.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || F_CPU != 160000000L
#error "Build NEXUS-S3 for the qualified ESP32-S3 160 MHz profile."
#endif

namespace {

constexpr char kVersion[] = "0.1.0-dev.3";
nexus::ManagementNetwork network;
nexus::ManagementService service(network);
char buildId[65]{};
unsigned cores = 0;
int resetReason = 0;
bool telemetryEnabled = false;
uint64_t nextSample = 0;
uint64_t nextSerial = 0;
uint32_t sequence = 0;

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

}  // namespace

const char* nexusVersion() { return kVersion; }
const char* nexusBuildId() { return buildId; }
unsigned nexusCoreCount() { return cores; }
int nexusResetReason() { return resetReason; }

// The pinned build redirects Arduino's startup hook here to leave NVS idle.
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

  network.preparePassword();
  nexus::initTemperatureSensor();
  const auto initial = nexus::sampleTelemetry();
  const auto faults = nexus::bootFaults({
      initial.cpuMHz, initial.flashBytes, initial.psramBytes, nexus::checkPartitionLayout(),
      true, initial.temperatureValid, initial.internal.freeBytes, initial.psram.freeBytes,
  });
  std::printf("BOOT chip=ESP32-S3 revision=%u cores=%u flash=%" PRIu32 " psram=%" PRIu32
              " mode=%s faults=0x%02" PRIX32 "\n",
              static_cast<unsigned>(chip.revision), static_cast<unsigned>(chip.cores),
              initial.flashBytes, initial.psramBytes, faults == 0 ? "IDLE" : "SAFE_MODE", faults);
  printTelemetry(initial);
  if (faults != 0) {
    std::puts("Startup checks failed. Preserve this log and compare the build profile and boot fault flags.");
    return;
  }
  Serial0.begin(115200);
  publish(initial);
  telemetryEnabled = true;
  std::puts("UART commands: A starts management, S stops management, K rotates the operator code.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!telemetryEnabled) return;
  const auto now = nowMs();
  const auto before = service.state();
  const int command = Serial0.available() > 0 ? Serial0.read() : -1;
  const bool browserStop = command != 'A' && command != 'S' && network.takeStop(now);
  if (command == 'A') {
    if (before == nexus::ServiceState::Stopped && !checkHeap()) return;
    const auto sample = nexus::sampleTelemetry();
    std::printf("MANAGEMENT START %s\n", nexus::reasonCode(service.start(guardSample(sample))));
    nextSample = 0;
  } else if (command == 'S' || browserStop) {
    std::printf("MANAGEMENT STOP SOURCE %s\n", browserStop ? "BROWSER" : "UART");
    std::printf("MANAGEMENT STOP %s\n", nexus::reasonCode(service.stop(now, nexus::Reason::StopRequested)));
    nextSample = 0;
  } else if (command == 'K' && before == nexus::ServiceState::Running) {
    network.rotateOperatorCode();
  }
  if (now < nextSample) return;
  auto sample = nexus::sampleTelemetry();
  service.tick(guardSample(sample));
  if (before == nexus::ServiceState::Running && service.state() != before) {
    std::printf("GUARD %s\n", nexus::reasonCode(service.lastReason()));
    if (service.state() == nexus::ServiceState::Stopped && !checkHeap()) return;
    sample = nexus::sampleTelemetry();
  }
  publish(sample);
  nextSample = sample.uptimeMs + 1000;
  if (now >= nextSerial) {
    printTelemetry(sample);
    nextSerial = now + 5000;
  }
}
