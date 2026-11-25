#pragma once

#include "guard.h"
#include "hal.h"
#include "stop_job.h"

#include <atomic>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

namespace nexus {

struct PublicState {
  Telemetry telemetry{};
  uint32_t sequence = 0;
  ServiceState service = ServiceState::Stopped;
  Reason reason = Reason::None;
  Reason warning = Reason::None;
  float cutoff = 0;
  bool leased = false;
};

class ManagementNetwork final : public ServiceDriver {
 public:
  void preparePassword();
  bool start() override;
  bool stop() override;
  void publish(const PublicState& state);
  bool takeStop(uint64_t nowMs);
  void rotateOperatorCode();

 private:
  bool startHttp();
  static void observeApStop(void* context, esp_event_base_t, int32_t, void*);
  static esp_err_t handle(httpd_req_t* request);
  esp_err_t route(httpd_req_t* request);
  esp_err_t get(httpd_req_t* request);
  esp_err_t mutate(httpd_req_t* request);
  esp_err_t login(httpd_req_t* request);
  esp_err_t jobs(httpd_req_t* request, bool cancel);
  bool authenticated(httpd_req_t* request);

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  struct Event {
    uint64_t atMs;
    ServiceState state;
    Reason reason;
  };
  PublicState snapshot_{};
  Event events_[8]{};
  unsigned eventCount_ = 0;
  unsigned nextEvent_ = 0;
  StopJob job_;
  unsigned pollingSeconds_ = 3;
  esp_netif_t* ap_ = nullptr;
  httpd_handle_t http_ = nullptr;
  bool infrastructureReady_ = false;
  bool wifiInitialized_ = false;
  bool apAttached_ = false;
  bool wifiStarted_ = false;
  esp_event_handler_instance_t stopListener_ = nullptr;
  std::atomic<bool> apStopped_{false};
  char password_[33]{};
  char operatorCode_[33]{};
  char session_[33]{};
  char host_[16]{};
  char origin_[24]{};
  uint64_t nextLoginMs_ = 0;
  uint64_t sessionExpiresMs_ = 0;
};

}  // namespace nexus
