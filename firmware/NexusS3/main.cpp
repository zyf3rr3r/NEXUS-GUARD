#include "boot_policy.h"
#include "hal.h"

#include <cinttypes>
#include <cstdio>
#include "esp32-hal-cpu.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || F_CPU != 160000000L
#error "Build NEXUS-S3 for the qualified ESP32-S3 160 MHz profile."
#endif

namespace {

constexpr char kVersion[] = "0.1.0-dev.1";
bool telemetryEnabled = false;

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

}  // namespace

// The pinned build redirects Arduino's startup hook here to leave NVS idle.
extern "C" void __wrap_initArduino() {}

void setup() {
  if (!heap_caps_check_integrity_all(true)) {
    std::printf("NEXUS-S3 %s SAFE_MODE HEAP_INTEGRITY. Power off and preserve this log.\n", kVersion);
    return;
  }

  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  char buildId[65]{};
  esp_app_get_elf_sha256(buildId, sizeof(buildId));
  std::printf("NEXUS-S3 %s build=%s sdk=%s reset=%d policy=serial-only-v1\n",
              kVersion, buildId, esp_get_idf_version(), static_cast<int>(esp_reset_reason()));

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
  telemetryEnabled = true;
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(5000));
  if (telemetryEnabled) {
    printTelemetry(nexus::sampleTelemetry());
  }
}
