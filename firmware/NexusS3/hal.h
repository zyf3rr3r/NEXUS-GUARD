#pragma once

#include <cstdint>

namespace nexus {

struct MemorySample {
  uint32_t freeBytes;
  uint32_t largestBlockBytes;
  uint32_t minimumFreeBytes;
};

struct Telemetry {
  uint64_t uptimeMs;
  uint32_t cpuMHz;
  uint32_t flashBytes;
  uint32_t psramBytes;
  MemorySample internal;
  MemorySample psram;
  float temperatureC;
  bool temperatureValid;
};

void initTemperatureSensor();
Telemetry sampleTelemetry();
bool checkPartitionLayout();

}  // namespace nexus
