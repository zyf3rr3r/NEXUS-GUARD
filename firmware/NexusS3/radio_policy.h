#pragma once

#include "guard.h"
#include "hal.h"

#include <cmath>

namespace nexus {

inline const char* optionalRadioBlock(ServiceState state, Reason warning, float cutoff, const Telemetry& sample) {
  if (state != ServiceState::Running) return "RADIO_MANAGEMENT_REQUIRED";
  if (!sample.temperatureValid || !std::isfinite(sample.temperatureC)) return "RADIO_TEMP_UNAVAILABLE";
  if (warning != Reason::None || sample.temperatureC >= cutoff - 3.0f) return "RADIO_THERMAL_MARGIN";
  if (sample.internal.freeBytes < 128 * 1024 || sample.internal.largestBlockBytes < 48 * 1024 ||
      sample.psram.freeBytes < kPsramReserve) return "RADIO_MEMORY_MARGIN";
  return nullptr;
}

}  // namespace nexus
