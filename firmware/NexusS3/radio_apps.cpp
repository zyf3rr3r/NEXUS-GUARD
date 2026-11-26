#include "radio_apps.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace nexus {
namespace {

void copyName(char (&target)[33], const uint8_t* source, size_t length) {
  for (size_t i = 0; i < length && i < 32 && source[i] != 0; ++i) {
    target[i] = source[i] >= 32 && source[i] < 127 ? source[i] : '?';
  }
}

void runBleHost(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

bool radioOk(const char* operation, esp_err_t status) {
  if (status != ESP_OK) std::printf("RADIO SDK %s %s\n", operation, esp_err_to_name(status));
  return status == ESP_OK;
}

}  // namespace

bool RadioApps::init() {
  if (!initialized_) {
    initialized_ = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifiEvent, this) == ESP_OK;
  }
  portENTER_CRITICAL(&lock_);
  result_ = {};
  portEXIT_CRITICAL(&lock_);
  return initialized_;
}

const char* RadioApps::queue(Kind kind, uint64_t now, const char* block) {
  portENTER_CRITICAL(&lock_);
  const char* reason = block;
  if (!initialized_ || faulted_) reason = "RADIO_DRIVER_UNAVAILABLE";
  else if (result_.state == State::Queued || result_.state == State::Running) reason = "RADIO_BUSY";
  else if (!reason && now < nextRequest_) reason = "RADIO_COOLDOWN";
  if (!reason) {
    result_ = {};
    result_.kind = kind;
    result_.state = State::Queued;
    cancel_ = false;
    nextRequest_ = now + 12000;
  }
  portEXIT_CRITICAL(&lock_);
  return reason;
}

void RadioApps::requestCancel() {
  portENTER_CRITICAL(&lock_);
  cancel_ = true;
  portEXIT_CRITICAL(&lock_);
}

RadioApps::Result RadioApps::result() {
  portENTER_CRITICAL(&lock_);
  const auto copy = result_;
  portEXIT_CRITICAL(&lock_);
  return copy;
}

const char* RadioApps::stateName(State state) {
  switch (state) {
    case State::Idle: return "idle";
    case State::Queued: return "queued";
    case State::Running: return "running";
    case State::Done: return "done";
    case State::Failed: return "failed";
    case State::Cancelled: return "cancelled";
  }
  return "failed";
}

void RadioApps::wifiEvent(void* context, esp_event_base_t, int32_t, void* data) {
  auto& apps = *static_cast<RadioApps*>(context);
  const auto* event = static_cast<wifi_event_sta_scan_done_t*>(data);
  if (event) apps.wifiDone_.store(static_cast<int>(event->status));
}

int RadioApps::bleEvent(ble_gap_event* event, void* context) {
  auto& tools = *static_cast<RadioApps*>(context);
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    tools.bleDone_.store(event->disc_complete.reason);
    return 0;
  }
  if (event->type != BLE_GAP_EVENT_DISC) return 0;
  ble_hs_adv_fields fields{};
  if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) return 0;
  Row row{};
  if (fields.name) copyName(row.name, fields.name, fields.name_len);
  row.rssi = event->disc.rssi;
  portENTER_CRITICAL(&tools.lock_);
  auto& result = tools.result_;
  if (result.kind == Kind::Ble && result.state == State::Running && result.count < kMaxRows) {
    result.rows[result.count++] = row;
  }
  portEXIT_CRITICAL(&tools.lock_);
  return 0;
}

void RadioApps::csiEvent(void* context, wifi_csi_info_t* frame) {
  if (frame == nullptr || frame->buf == nullptr) return;
  auto& tools = *static_cast<RadioApps*>(context);
  portENTER_CRITICAL(&tools.lock_);
  auto& result = tools.result_;
  if (result.kind == Kind::Csi && result.state == State::Running && result.frames < 192 &&
      std::memcmp(frame->mac, tools.csiPeer_, sizeof(tools.csiPeer_)) == 0 &&
      std::memcmp(frame->dmac, tools.csiLocal_, sizeof(tools.csiLocal_)) == 0) {
    const unsigned first = frame->first_word_invalid ? 4 : 0;
    const unsigned end = std::min<unsigned>(frame->len, first + 64);
    if (end > first) {
      unsigned sum = 0;
      for (unsigned i = first; i < end; ++i) sum += std::abs(static_cast<int>(frame->buf[i]));
      const unsigned mean = sum / (end - first);
      if (result.frames == 0) result.minAmplitude = result.maxAmplitude = mean;
      result.minAmplitude = std::min(result.minAmplitude, mean);
      result.maxAmplitude = std::max(result.maxAmplitude, mean);
      result.amplitude += mean;
      ++result.frames;
    }
  }
  portEXIT_CRITICAL(&tools.lock_);
}

bool RadioApps::begin(Kind kind, uint64_t now) {
  if (kind == Kind::Wifi) {
    if (!radioOk("wifi_apsta", esp_wifi_set_mode(WIFI_MODE_APSTA))) return false;
    wifiModeChanged_ = true;
    wifi_scan_config_t config{};
    config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    config.scan_time.passive = 120;
    config.home_chan_dwell_time = 40;
    wifiDone_.store(-1);
    wifiScanRunning_ = radioOk("wifi_scan_start", esp_wifi_scan_start(&config, false));
    deadline_ = now + 6000;
    return wifiScanRunning_;
  }
  if (kind == Kind::Ble) {
    if (!bleResident_) {
      if (nimble_port_init() != ESP_OK) return false;
      bleResident_ = true;
      nimble_port_freertos_init(runBleHost);
    }
    deadline_ = now + 4000;
    return true;
  }
  wifi_sta_list_t stations{};
  uint8_t local[6]{};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num != 1 ||
      esp_wifi_get_mac(WIFI_IF_AP, local) != ESP_OK) return false;
  portENTER_CRITICAL(&lock_);
  std::memcpy(csiPeer_, stations.sta[0].mac, sizeof(csiPeer_));
  std::memcpy(csiLocal_, local, sizeof(csiLocal_));
  portEXIT_CRITICAL(&lock_);
  wifi_csi_config_t csi{};
  csi.lltf_en = true;
  csi.htltf_en = true;
  if (esp_wifi_set_csi_config(&csi) != ESP_OK ||
      esp_wifi_set_csi_rx_cb(csiEvent, this) != ESP_OK) return false;
  csiEnabled_ = esp_wifi_set_csi(true) == ESP_OK;
  if (!csiEnabled_) esp_wifi_set_csi_rx_cb(nullptr, nullptr);
  deadline_ = now + 3000;
  return csiEnabled_;
}

bool RadioApps::beginBleDiscovery(uint64_t now) {
  uint8_t addressType = 0;
  if (ble_hs_id_infer_auto(0, &addressType) != 0) return false;
  ble_gap_disc_params params{};
  params.passive = 1;
  params.filter_duplicates = 1;
  bleDone_.store(-1);
  bleScanning_ = ble_gap_disc(addressType, 2500, &params, bleEvent, this) == 0;
  deadline_ = now + 4000;
  return bleScanning_;
}

void RadioApps::finish(State state, const char* reason) {
  portENTER_CRITICAL(&lock_);
  result_.state = state;
  result_.reason = reason;
  portEXIT_CRITICAL(&lock_);
}

bool RadioApps::stop() {
  const auto snapshot = result();
  if (snapshot.state == State::Queued || snapshot.state == State::Running) finish(State::Cancelled, "RADIO_STOPPED");
  bool ok = true;
  if (wifiScanRunning_) {
    const bool stopped = esp_wifi_scan_stop() == ESP_OK || wifiDone_.load() >= 0;
    const bool cleared = esp_wifi_clear_ap_list() == ESP_OK;
    wifiScanRunning_ = !stopped;
    ok = ok && stopped && cleared;
  }
  if (wifiModeChanged_) {
    wifiModeChanged_ = esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK;
    ok = ok && !wifiModeChanged_;
  }
  if (csiEnabled_) {
    csiEnabled_ = esp_wifi_set_csi(false) != ESP_OK;
    const bool callbackCleared = esp_wifi_set_csi_rx_cb(nullptr, nullptr) == ESP_OK;
    ok = ok && !csiEnabled_ && callbackCleared;
  }
  if (bleScanning_) {
    const int result = ble_gap_disc_cancel();
    bleScanning_ = result != 0 && result != BLE_HS_EALREADY;
    ok = ok && !bleScanning_;
  }
  if (!ok) {
    portENTER_CRITICAL(&lock_);
    faulted_ = true;
    result_.state = State::Failed;
    result_.reason = "RADIO_RELEASE_UNCONFIRMED";
    portEXIT_CRITICAL(&lock_);
  }
  return ok;
}

void RadioApps::tick(uint64_t now, const char* block) {
  portENTER_CRITICAL(&lock_);
  const auto kind = result_.kind;
  const auto state = result_.state;
  const bool cancel = cancel_;
  portEXIT_CRITICAL(&lock_);
  if (state != State::Queued && state != State::Running) return;
  if (block || cancel) {
    const char* reason = block ? block : "RADIO_USER_CANCELLED";
    const bool released = stop();
    if (released) finish(State::Cancelled, reason);
    std::printf("RADIO %s %s\n", released ? "CANCELLED" : "STOP_FAILED", reason);
    return;
  }
  if (state == State::Queued) {
    finish(State::Running);
    if (!begin(kind, now) && stop()) finish(State::Failed, "RADIO_START_FAILED");
    return;
  }
  if (kind == Kind::Wifi && wifiDone_.load() >= 0) {
    wifi_ap_record_t records[kMaxRows]{};
    uint16_t count = kMaxRows;
    const int scanStatus = wifiDone_.load();
    bool ok = scanStatus == 0 && radioOk("wifi_scan_results", esp_wifi_scan_get_ap_records(&count, records));
    if (scanStatus != 0) std::printf("RADIO WIFI_SCAN_EVENT %d\n", scanStatus);
    if (!ok) esp_wifi_clear_ap_list();
    wifiScanRunning_ = false;
    const bool released = stop();
    ok = released && ok;
    portENTER_CRITICAL(&lock_);
    if (ok) {
      result_.count = count;
      for (unsigned i = 0; i < count; ++i) {
        auto& row = result_.rows[i];
        copyName(row.name, records[i].ssid, sizeof(records[i].ssid));
        row.rssi = records[i].rssi;
        row.channel = records[i].primary;
        row.security = records[i].authmode;
      }
    }
    result_.state = ok ? State::Done : State::Failed;
    result_.reason = ok ? "NONE" : released ? "RADIO_SCAN_FAILED" : "RADIO_RELEASE_UNCONFIRMED";
    portEXIT_CRITICAL(&lock_);
    std::printf("RADIO WIFI %s count=%u\n", ok ? "DONE" : "FAILED", ok ? count : 0);
    return;
  }
  if (kind == Kind::Ble) {
    if (!bleScanning_ && ble_hs_synced() && now < deadline_) {
      if (!beginBleDiscovery(now) && stop()) finish(State::Failed, "RADIO_START_FAILED");
      return;
    }
    if (bleScanning_ && bleDone_.load() >= 0) {
      bleScanning_ = false;
      if (bleDone_.load() == 0) finish(State::Done);
      else finish(State::Failed, "RADIO_SCAN_FAILED");
      return;
    }
  }
  if (now >= deadline_) {
    const bool stopped = stop();
    if (stopped) {
      if (kind == Kind::Csi) finish(State::Done);
      else finish(State::Failed, "RADIO_TIMEOUT");
    }
  }
}

}  // namespace nexus
