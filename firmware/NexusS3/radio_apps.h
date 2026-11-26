#pragma once

#include <atomic>
#include <cstdint>
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

struct ble_gap_event;

namespace nexus {

class RadioApps {
 public:
  enum class Kind { Wifi, Ble, Csi };
  enum class State { Idle, Queued, Running, Done, Failed, Cancelled };
  struct Row {
    char name[33]{};
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint8_t security = 0;
  };
  static constexpr unsigned kMaxRows = 12;
  struct Result {
    Kind kind = Kind::Wifi;
    State state = State::Idle;
    const char* reason = "NONE";
    unsigned count = 0;
    unsigned frames = 0;
    unsigned amplitude = 0;
    unsigned minAmplitude = 0;
    unsigned maxAmplitude = 0;
    Row rows[kMaxRows]{};
  };

  bool init();
  const char* queue(Kind kind, uint64_t now, const char* block);
  void requestCancel();
  void tick(uint64_t now, const char* block);
  bool stop();
  Result result();
  static const char* stateName(State state);

 private:
  static void wifiEvent(void* context, esp_event_base_t, int32_t, void* data);
  static int bleEvent(ble_gap_event* event, void* context);
  static void csiEvent(void* context, wifi_csi_info_t* frame);
  bool begin(Kind kind, uint64_t now);
  bool beginBleDiscovery(uint64_t now);
  void finish(State state, const char* reason = "NONE");

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  Result result_{};
  bool cancel_ = false;
  bool initialized_ = false;
  bool faulted_ = false;
  bool wifiModeChanged_ = false;
  bool wifiScanRunning_ = false;
  bool csiEnabled_ = false;
  bool bleResident_ = false;
  bool bleScanning_ = false;
  uint8_t csiPeer_[6]{};
  uint8_t csiLocal_[6]{};
  uint64_t deadline_ = 0;
  uint64_t nextRequest_ = 0;
  std::atomic<int> wifiDone_{-1};
  std::atomic<int> bleDone_{-1};
};

}  // namespace nexus
