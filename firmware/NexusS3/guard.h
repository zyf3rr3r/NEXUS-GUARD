#pragma once

#include "boot_policy.h"

namespace nexus {

enum class Reason {
  None, TemperatureUnavailable, WarmStart, ThermalWarning, ThermalCutoff,
  HeapReserve, PsramReserve, BlockTooSmall, Cooldown, SessionExpired,
  Busy, DriverStart, DriverStop, HeapIntegrity, StopRequested,
};

const char* reasonCode(Reason reason);

struct GuardSample {
  uint64_t nowMs;
  float temperatureC;
  bool temperatureValid;
  uint32_t internalFree;
  uint32_t internalLargest;
  uint32_t psramFree;
};

class Guard {
 public:
  static constexpr float kColdStartC = 33.3f;
  static constexpr uint64_t kSessionMs = 600000;
  static constexpr uint32_t kManagementHeadroom = 72 * 1024;
  static constexpr uint32_t kManagementLargest = 32 * 1024;
  static constexpr uint64_t kCooldownMs = 30000;

  Reason admit(const GuardSample& sample) const;
  void acquire(const GuardSample& sample);
  Reason supervise(const GuardSample& sample) const;
  void release(uint64_t nowMs);
  bool leased() const { return leased_; }
  bool cooling(uint64_t nowMs) const { return nowMs < cooldownUntil_; }
  float cutoff() const { return cutoff_; }

 private:
  bool leased_ = false;
  float cutoff_ = 0;
  uint64_t expires_ = 0;
  uint64_t cooldownUntil_ = 0;
};

enum class ServiceState { Stopped, Starting, Running, Stopping, Faulted };
const char* serviceStateName(ServiceState state);

class ServiceDriver {
 public:
  virtual bool start() = 0;
  virtual bool stop() = 0;  // True only after all service-owned resources are released.
 protected:
  ~ServiceDriver() = default;
};

class ManagementService {
 public:
  explicit ManagementService(ServiceDriver& driver) : driver_(driver) {}
  Reason start(const GuardSample& sample);
  Reason stop(uint64_t nowMs, Reason reason);
  void tick(const GuardSample& sample);
  void latchIntegrityFault();
  ServiceState state() const { return state_; }
  Reason lastReason() const { return lastReason_; }
  Reason warning() const { return warning_; }
  const Guard& guard() const { return guard_; }

 private:
  ServiceDriver& driver_;
  Guard guard_;
  ServiceState state_ = ServiceState::Stopped;
  Reason lastReason_ = Reason::None;
  Reason warning_ = Reason::None;
};

}  // namespace nexus
