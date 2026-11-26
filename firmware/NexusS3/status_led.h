#pragma once

#include "guard.h"

#include <cstdint>

namespace nexus {

class StatusLed {
 public:
  void init();
  void notifyDenied(uint64_t nowMs);
  void notifySaved(uint64_t nowMs);
  void update(ServiceState state, Reason warning, bool holdingToStart, uint64_t nowMs);

 private:
  void setRgb(uint8_t r, uint8_t g, uint8_t b);

  bool initialized_ = false;
  uint8_t lastR_ = 255;
  uint8_t lastG_ = 255;
  uint8_t lastB_ = 255;
  uint64_t deniedUntilMs_ = 0;
  uint64_t savedUntilMs_ = 0;
};

}  // namespace nexus
