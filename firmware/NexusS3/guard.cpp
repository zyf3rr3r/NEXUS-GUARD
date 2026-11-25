#include "guard.h"

#include <cmath>

namespace nexus {
namespace {
bool validTemperature(const GuardSample& sample) {
  return sample.temperatureValid && std::isfinite(sample.temperatureC) &&
         sample.temperatureC >= 10 && sample.temperatureC <= 50;
}
}  // namespace

const char* reasonCode(Reason reason) {
  switch (reason) {
    case Reason::None: return "NONE";
    case Reason::TemperatureUnavailable: return "TEMP_UNAVAILABLE";
    case Reason::WarmStart: return "THERMAL_ADMISSION_DENIED";
    case Reason::ThermalWarning: return "THERMAL_WARNING";
    case Reason::ThermalCutoff: return "THERMAL_CUTOFF";
    case Reason::HeapReserve: return "HEAP_RESERVE";
    case Reason::PsramReserve: return "PSRAM_RESERVE";
    case Reason::BlockTooSmall: return "BLOCK_TOO_SMALL";
    case Reason::Cooldown: return "COOLDOWN";
    case Reason::SessionExpired: return "SESSION_EXPIRED";
    case Reason::Busy: return "SERVICE_CONFLICT";
    case Reason::DriverStart: return "DRIVER_START_FAILED";
    case Reason::DriverStop: return "DRIVER_STOP_UNCONFIRMED";
    case Reason::HeapIntegrity: return "HEAP_INTEGRITY";
    case Reason::StopRequested: return "STOP_REQUESTED";
  }
  return "UNKNOWN";
}

Reason Guard::admit(const GuardSample& sample) const {
  if (leased_) return Reason::Busy;
  if (cooling(sample.nowMs)) return Reason::Cooldown;
  if (!validTemperature(sample)) return Reason::TemperatureUnavailable;
  if (sample.temperatureC > kColdStartC) return Reason::WarmStart;
  if (sample.internalFree < kInternalReserve + kManagementHeadroom) return Reason::HeapReserve;
  if (sample.psramFree < kPsramReserve) return Reason::PsramReserve;
  if (sample.internalLargest < kManagementLargest) return Reason::BlockTooSmall;
  return Reason::None;
}

void Guard::acquire(const GuardSample& sample) {
  leased_ = true;
  cutoff_ = sample.temperatureC + 10.0f;  // Cold-start admission caps this below 55 °C.
  expires_ = sample.nowMs + kSessionMs;
}

Reason Guard::supervise(const GuardSample& sample) const {
  if (!leased_) return Reason::None;
  if (!validTemperature(sample)) return Reason::TemperatureUnavailable;
  if (sample.temperatureC >= cutoff_) return Reason::ThermalCutoff;
  if (sample.internalFree < kInternalReserve) return Reason::HeapReserve;
  if (sample.psramFree < kPsramReserve) return Reason::PsramReserve;
  if (sample.nowMs >= expires_) return Reason::SessionExpired;
  if (sample.temperatureC >= cutoff_ - 2.0f) return Reason::ThermalWarning;
  return Reason::None;
}

void Guard::release(uint64_t nowMs) {
  leased_ = false;
  cooldownUntil_ = nowMs + kCooldownMs;
}

const char* serviceStateName(ServiceState state) {
  switch (state) {
    case ServiceState::Stopped: return "stopped";
    case ServiceState::Starting: return "starting";
    case ServiceState::Running: return "running";
    case ServiceState::Stopping: return "stopping";
    case ServiceState::Faulted: return "faulted";
  }
  return "unknown";
}

Reason ManagementService::start(const GuardSample& sample) {
  if (state_ == ServiceState::Faulted) return lastReason_;
  if (state_ != ServiceState::Stopped) return Reason::Busy;
  lastReason_ = guard_.admit(sample);
  if (lastReason_ != Reason::None) return lastReason_;
  guard_.acquire(sample);
  state_ = ServiceState::Starting;
  if (!driver_.start()) {
    state_ = ServiceState::Stopping;
    const bool released = driver_.stop();
    if (released) guard_.release(sample.nowMs);
    state_ = ServiceState::Faulted;
    return lastReason_ = released ? Reason::DriverStart : Reason::DriverStop;
  }
  state_ = ServiceState::Running;
  return Reason::None;
}

Reason ManagementService::stop(uint64_t nowMs, Reason reason) {
  if (state_ == ServiceState::Faulted) return lastReason_;
  if (state_ == ServiceState::Stopped) return Reason::None;
  if (state_ != ServiceState::Running) return Reason::Busy;
  state_ = ServiceState::Stopping;
  warning_ = Reason::None;
  if (!driver_.stop()) {
    state_ = ServiceState::Faulted;
    return lastReason_ = Reason::DriverStop;
  }
  guard_.release(nowMs);
  state_ = ServiceState::Stopped;
  lastReason_ = reason;
  return Reason::None;
}

void ManagementService::tick(const GuardSample& sample) {
  if (state_ != ServiceState::Running) return;
  const auto reason = guard_.supervise(sample);
  if (reason != Reason::None && reason != Reason::ThermalWarning) {
    stop(sample.nowMs, reason);
  } else if (reason == Reason::ThermalWarning || sample.temperatureC < guard_.cutoff() - 3.0f) {
    warning_ = reason;
  }
}

void ManagementService::latchIntegrityFault() {
  state_ = ServiceState::Faulted;
  lastReason_ = Reason::HeapIntegrity;
}

}  // namespace nexus
