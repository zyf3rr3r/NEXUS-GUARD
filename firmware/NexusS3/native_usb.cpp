#include "native_usb.h"
#include "media_arm.h"

#if !ARDUINO_USB_MODE
#include "USB.h"
#include "USBCDC.h"
#include "USBHIDConsumerControl.h"

namespace {
USBCDC console;
USBHIDConsumerControl media;
nexus::MediaArm arm;
bool ready = false;
}  // namespace
#endif

namespace nexus {

void setupNativeUsb() {
#if !ARDUINO_USB_MODE
  console.begin(115200);
  console.enableReboot(false);
  media.begin();
  ready = USB.begin();
#endif
}

void pollNativeUsb(uint64_t nowMs, bool allowed) {
#if !ARDUINO_USB_MODE
  if (!ready || !console || !allowed) {
    arm.clear();
    return;
  }
  if (!console.available()) return;
  const int command = console.read();
  if (command == '?') {
    console.println("NEXUS-S3 native CDC. U arms one media action. P play/pause, N next, B previous, X disarms.");
    return;
  }
  if (command == 'U') {
    arm.arm(nowMs);
    console.println("ARMED for one media action.");
    return;
  }
  if (command == 'X') {
    arm.clear();
    console.println("DISARMED.");
    return;
  }
  const uint16_t key = command == 'P' ? CONSUMER_CONTROL_PLAY_PAUSE :
                       command == 'N' ? CONSUMER_CONTROL_SCAN_NEXT :
                       command == 'B' ? CONSUMER_CONTROL_SCAN_PREVIOUS : 0;
  const bool armed = arm.consume(nowMs);
  if (key == 0 || !armed) {
    console.println("Action rejected. Check arming and command.");
    return;
  }
  const bool pressed = media.press(key) != 0;
  const bool released = media.release() != 0;
  console.println(pressed && released ? "Media action sent and released." : "Media action unconfirmed.");
#else
  (void)nowMs;
  (void)allowed;
#endif
}

}  // namespace nexus
