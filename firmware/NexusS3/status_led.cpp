#include "status_led.h"

#include "Arduino.h"

namespace nexus {
namespace {
constexpr uint8_t kLedPin = PIN_RGB_LED;
}

void StatusLed::init() {
  initialized_ = true;
  setRgb(0, 0, 0);
}

void StatusLed::notifyDenied(uint64_t nowMs) {
  deniedUntilMs_ = nowMs + 900;
}

void StatusLed::notifySaved(uint64_t nowMs) {
  savedUntilMs_ = nowMs + 450;
}

void StatusLed::setRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!initialized_) return;
  if (r == lastR_ && g == lastG_ && b == lastB_) return;
  lastR_ = r;
  lastG_ = g;
  lastB_ = b;
  rgbLedWrite(kLedPin, r, g, b);
}

void StatusLed::update(ServiceState state, Reason warning, bool holdingToStart, uint64_t nowMs) {
  if (nowMs < deniedUntilMs_) {
    const bool on = ((deniedUntilMs_ - nowMs) / 150) % 2 == 1;
    setRgb(on ? 36 : 0, 0, 0);
    return;
  }
  if (nowMs < savedUntilMs_) {
    setRgb(0, 22, 24);
    return;
  }
  if (state == ServiceState::Faulted) {
    const bool on = (nowMs / 500) % 2 == 0;
    setRgb(on ? 36 : 0, 0, 0);
    return;
  }
  if (holdingToStart) {
    setRgb(0, 10, 32);
    return;
  }
  if (state == ServiceState::Running || state == ServiceState::Starting) {
    if (warning == Reason::ThermalWarning) {
      setRgb(32, 16, 0);
    } else {
      setRgb(0, 26, 0);
    }
    return;
  }
  setRgb(0, 0, 0);
}

}  // namespace nexus
