#pragma once

#include <cstdint>

namespace nexus {

constexpr uint32_t kCpuMHz = 160;
constexpr uint32_t kFlashBytes = 16u * 1024 * 1024;
constexpr uint32_t kPsramBytes = 8u * 1024 * 1024;
constexpr uint32_t kInternalReserve = 64u * 1024;
constexpr uint32_t kPsramReserve = 1024u * 1024;

struct BootSample {
  uint32_t cpuMHz;
  uint32_t flashBytes;
  uint32_t psramBytes;
  bool partitionLayoutMatches;
  bool heapIntact;
  bool temperatureValid;
  uint32_t internalFreeBytes;
  uint32_t psramFreeBytes;
};

enum class BootFault : uint32_t {
  ClockMismatch = 1u << 0,
  FlashMismatch = 1u << 1,
  PsramMismatch = 1u << 2,
  PartitionMismatch = 1u << 3,
  HeapIntegrity = 1u << 4,
  TemperatureUnavailable = 1u << 5,
  HeapReserve = 1u << 6,
  PsramReserve = 1u << 7,
};

constexpr uint32_t bootFaults(const BootSample& sample) {
  return (sample.cpuMHz == kCpuMHz ? 0u : uint32_t(BootFault::ClockMismatch)) |
         (sample.flashBytes == kFlashBytes ? 0u : uint32_t(BootFault::FlashMismatch)) |
         (sample.psramBytes == kPsramBytes ? 0u : uint32_t(BootFault::PsramMismatch)) |
         (sample.partitionLayoutMatches ? 0u : uint32_t(BootFault::PartitionMismatch)) |
         (sample.heapIntact ? 0u : uint32_t(BootFault::HeapIntegrity)) |
         (sample.temperatureValid ? 0u : uint32_t(BootFault::TemperatureUnavailable)) |
         (sample.internalFreeBytes >= kInternalReserve ? 0u : uint32_t(BootFault::HeapReserve)) |
         (sample.psramFreeBytes >= kPsramReserve ? 0u : uint32_t(BootFault::PsramReserve));
}

}  // namespace nexus
