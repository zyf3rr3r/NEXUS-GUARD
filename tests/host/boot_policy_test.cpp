#include "../../firmware/NexusS3/boot_policy.h"

#include <cstdio>

int main() {
  using namespace nexus;
  constexpr BootSample healthy{160, 16777216, 8388608, true, true, true, 65536, 1048576};
  constexpr BootSample allInvalid{240, 0, 0, false, false, false, 65535, 1048575};
  struct TestCase {
    const char* name;
    BootSample sample;
    uint32_t expected;
  };
  constexpr TestCase tests[] = {
      {"qualified configuration at the reserve boundaries", healthy, 0},
      {"wrong clock", {240, 16777216, 8388608, true, true, true, 65536, 1048576},
       uint32_t(BootFault::ClockMismatch)},
      {"wrong flash", {160, 4194304, 8388608, true, true, true, 65536, 1048576},
       uint32_t(BootFault::FlashMismatch)},
      {"missing PSRAM", {160, 16777216, 0, true, true, true, 65536, 1048576},
       uint32_t(BootFault::PsramMismatch)},
      {"unexpected partitions", {160, 16777216, 8388608, false, true, true, 65536, 1048576},
       uint32_t(BootFault::PartitionMismatch)},
      {"heap integrity failure", {160, 16777216, 8388608, true, false, true, 65536, 1048576},
       uint32_t(BootFault::HeapIntegrity)},
      {"unavailable temperature", {160, 16777216, 8388608, true, true, false, 65536, 1048576},
       uint32_t(BootFault::TemperatureUnavailable)},
      {"insufficient internal heap", {160, 16777216, 8388608, true, true, true, 65535, 1048576},
       uint32_t(BootFault::HeapReserve)},
      {"insufficient PSRAM", {160, 16777216, 8388608, true, true, true, 65536, 1048575},
       uint32_t(BootFault::PsramReserve)},
      {"simultaneous failures", allInvalid, 0xff},
  };
  unsigned failed = 0;
  for (const auto& test : tests) {
    const bool passed = bootFaults(test.sample) == test.expected;
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", test.name);
    failed += !passed;
  }
  return failed == 0 ? 0 : 1;
}
