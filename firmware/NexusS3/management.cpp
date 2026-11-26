#include "management.h"
#include "radio_policy.h"
#include "web_assets.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

extern const char* nexusVersion();
extern const char* nexusBuildId();
extern unsigned nexusCoreCount();
extern int nexusResetReason();

namespace nexus {
namespace {

constexpr char kSsid[] = "NEXUS-S3";
constexpr char kApps[] = R"json([
 {"id":"pocketos","name":"PocketOS","state":"available"},
 {"id":"chipvitals","name":"ChipVitals","state":"available"},
 {"id":"settings","name":"Settings","state":"available"},
 {"id":"airscope","name":"AirScope","state":"available","detail":"Passive Wi-Fi discovery"},
 {"id":"bluescope","name":"BlueScope","state":"available","detail":"Passive BLE discovery"},
 {"id":"cyberdeck","name":"CyberDeck","state":"available","detail":"NEXUS-S3 diagnostics"},
 {"id":"deaddrop","name":"DeadDrop","state":"available","detail":"Local session inbox"},
 {"id":"pocketvault","name":"PocketVault","state":"available","detail":"Encrypted local notes"},
 {"id":"toolbox","name":"Toolbox","state":"available","detail":"Offline text utilities"},
 {"id":"usb-studio","name":"USB Studio","state":"available","detail":"Browser serial monitor"},
 {"id":"ghostlink","name":"GhostLink","state":"available","detail":"Armed serial shortcuts"},
 {"id":"soundforge","name":"SoundForge","state":"available","detail":"Browser tone sequencer"},
 {"id":"portalplay","name":"PortalPlay","state":"available","detail":"Offline memory game"},
 {"id":"ghostsense","name":"GhostSense","state":"available","detail":"Experimental CSI capture"},
 {"id":"cyberarena","name":"CyberArena","state":"available","detail":"Offline defensive puzzles"}
])json";

uint64_t nowMs() { return esp_timer_get_time() / 1000; }

void randomHex(char (&output)[33]) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    output[2 * i] = digits[bytes[i] >> 4];
    output[2 * i + 1] = digits[bytes[i] & 15];
  }
  output[32] = '\0';
  mbedtls_platform_zeroize(bytes, sizeof(bytes));
}

bool matchesSecret(const char* first, const char* second) {
  unsigned difference = 0;
  for (unsigned i = 0; i < 32; ++i) difference |= first[i] ^ second[i];
  return difference == 0;
}

bool headerIs(httpd_req_t* req, const char* header, const char* expected) {
  char value[72]{};
  return httpd_req_get_hdr_value_str(req, header, value, sizeof(value)) == ESP_OK &&
         std::strcmp(value, expected) == 0;
}

esp_err_t send(httpd_req_t* req, const char* status, const char* type, const char* payload,
               ssize_t length = HTTPD_RESP_USE_STRLEN) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
                     "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  return httpd_resp_send(req, payload, length);
}

esp_err_t json(httpd_req_t* req, const char* payload, const char* status = "200 OK") {
  return send(req, status, "application/json", payload);
}

esp_err_t error(httpd_req_t* req, const char* status, const char* code) {
  char payload[120]{};
  std::snprintf(payload, sizeof(payload), R"({"error":{"code":"%s"}})", code);
  return json(req, payload, status);
}

bool readBody(httpd_req_t* req, char* body, size_t length) {
  return req->content_len == length &&
         headerIs(req, "Content-Type", "text/plain") &&
         httpd_req_recv(req, body, length) == static_cast<int>(length);
}

void jsonAscii(const char* input, char* output) {
  while (*input) {
    if (*input == '"' || *input == '\\') *output++ = '\\';
    *output++ = *input++;
  }
  *output = '\0';
}

const char* radioName(RadioApps::Kind kind) {
  switch (kind) {
    case RadioApps::Kind::Wifi: return "wifi";
    case RadioApps::Kind::Ble: return "ble";
    case RadioApps::Kind::Csi: return "csi";
  }
  return "unknown";
}

bool accepted(esp_err_t result) {
  if (result != ESP_OK) std::printf("MANAGEMENT SDK_ERROR %s\n", esp_err_to_name(result));
  return result == ESP_OK;
}

}  // namespace

void ManagementNetwork::preparePassword() {
  // The one boot-time call precedes Wi-Fi and ADC users.
  bootloader_random_enable();
  randomHex(password_);
  bootloader_random_disable();
}

bool ManagementNetwork::start() {
  if (password_[0] == '\0') return false;
  if (!infrastructureReady_) {
    if (!accepted(esp_netif_init()) || !accepted(esp_event_loop_create_default())) return false;
    infrastructureReady_ = true;
  }
  if (!radioApps_.init()) std::puts("MANAGEMENT RADIO_APPS_UNAVAILABLE");
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  init.nvs_enable = 0;
  if (!accepted(esp_wifi_init(&init))) return false;
  wifiInitialized_ = true;
  const esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_WIFI_AP();
  ap_ = esp_netif_new(&netifConfig);
  if (ap_ == nullptr) return false;
  if (!accepted(esp_netif_attach_wifi_ap(ap_))) return false;
  apAttached_ = true;
  if (!accepted(esp_wifi_set_default_wifi_ap_handlers())) return false;
  if (!accepted(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                                    observeApStop, this, &stopListener_))) return false;

  wifi_config_t config{};
  std::memcpy(config.ap.ssid, kSsid, sizeof(kSsid) - 1);
  config.ap.ssid_len = sizeof(kSsid) - 1;
  std::memcpy(config.ap.password, password_, sizeof(password_));
  config.ap.channel = 1;
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  config.ap.max_connection = 1;
  config.ap.pmf_cfg.capable = true;
  const bool configured = accepted(esp_wifi_set_storage(WIFI_STORAGE_RAM)) &&
                          accepted(esp_wifi_set_mode(WIFI_MODE_AP)) &&
                          accepted(esp_wifi_set_config(WIFI_IF_AP, &config));
  mbedtls_platform_zeroize(config.ap.password, sizeof(config.ap.password));
  if (!configured || !accepted(esp_wifi_start())) return false;
  wifiStarted_ = true;
  int8_t power = 0;
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_netif_ip_info_t address{};
  if (!accepted(esp_wifi_get_mode(&mode)) || mode != WIFI_MODE_AP ||
      !accepted(esp_wifi_set_max_tx_power(34)) ||
      !accepted(esp_wifi_get_max_tx_power(&power)) || power != 34 ||
      !accepted(esp_netif_get_ip_info(ap_, &address)) || address.ip.addr == 0) return false;
  std::snprintf(host_, sizeof(host_), IPSTR, IP2STR(&address.ip));
  std::snprintf(origin_, sizeof(origin_), "http://%s", host_);
  if (!startHttp()) return false;
  std::printf("MANAGEMENT URL %s  SSID %s  WPA2 password %s\n", origin_, kSsid, password_);
  rotateOperatorCode();
  return true;
}

bool ManagementNetwork::stop() {
  if (http_ != nullptr) {
    if (!accepted(httpd_stop(http_))) return false;
    http_ = nullptr;
  }
  const bool radioReleased = radioApps_.stop();
  if (wifiStarted_) {
    const auto deadline = nowMs() + 1500;
    apStopped_.store(false);
    if (!accepted(esp_wifi_stop())) return false;
    while (!apStopped_.load() && nowMs() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    if (!apStopped_.load()) {
      std::puts("MANAGEMENT AP_STOP_TIMEOUT");
      return false;
    }
    wifiStarted_ = false;
  }
  if (stopListener_ != nullptr) {
    if (!accepted(esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_AP_STOP, stopListener_))) return false;
    stopListener_ = nullptr;
  }
  if (apAttached_) {
    if (!accepted(esp_wifi_clear_default_wifi_driver_and_handlers(ap_))) return false;
    apAttached_ = false;
  }
  if (ap_ != nullptr) {
    esp_netif_destroy(ap_);
    ap_ = nullptr;
  }
  if (wifiInitialized_) {
    if (!accepted(esp_wifi_deinit())) return false;
    wifiInitialized_ = false;
  }
  portENTER_CRITICAL(&lock_);
  mbedtls_platform_zeroize(operatorCode_, sizeof(operatorCode_));
  mbedtls_platform_zeroize(session_, sizeof(session_));
  sessionExpiresMs_ = 0;
  job_.clear();
  portEXIT_CRITICAL(&lock_);
  if (radioReleased) std::puts("MANAGEMENT STOP CONFIRMED");
  return radioReleased;
}

void ManagementNetwork::observeApStop(void* context, esp_event_base_t, int32_t, void*) {
  static_cast<ManagementNetwork*>(context)->apStopped_.store(true);
}

void ManagementNetwork::publish(const PublicState& state) {
  portENTER_CRITICAL(&lock_);
  if (state.service != snapshot_.service || state.reason != snapshot_.reason || state.warning != snapshot_.warning) {
    events_[nextEvent_] = {state.telemetry.uptimeMs, state.service,
                          state.warning == Reason::None ? state.reason : state.warning};
    nextEvent_ = (nextEvent_ + 1) % 8;
    if (eventCount_ < 8) ++eventCount_;
  }
  snapshot_ = state;
  portEXIT_CRITICAL(&lock_);
}

void ManagementNetwork::tickApps(uint64_t now, const char* block) {
  radioApps_.tick(now, block);
}

const char* ManagementNetwork::queueRadio(RadioApps::Kind kind, uint64_t now) {
  portENTER_CRITICAL(&lock_);
  const auto sample = snapshot_;
  portEXIT_CRITICAL(&lock_);
  return radioApps_.queue(kind, now, optionalRadioBlock(sample.service, sample.warning, sample.cutoff, sample.telemetry));
}

bool ManagementNetwork::takeStop(uint64_t now) {
  portENTER_CRITICAL(&lock_);
  const bool due = job_.take(now);
  portEXIT_CRITICAL(&lock_);
  return due;
}

void ManagementNetwork::rotateOperatorCode() {
  char code[33]{};
  randomHex(code);
  portENTER_CRITICAL(&lock_);
  std::memcpy(operatorCode_, code, sizeof(code));
  mbedtls_platform_zeroize(session_, sizeof(session_));
  sessionExpiresMs_ = nextLoginMs_ = 0;
  portEXIT_CRITICAL(&lock_);
  std::printf("MANAGEMENT one-time operator code %s\n", code);
  mbedtls_platform_zeroize(code, sizeof(code));
}

bool ManagementNetwork::startHttp() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 4;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 2;
  config.stack_size = 8192;
  if (!accepted(httpd_start(&http_, &config))) return false;
  const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PATCH, HTTP_DELETE};
  for (const auto method : methods) {
    httpd_uri_t route{};
    route.uri = "/*";
    route.method = method;
    route.handler = handle;
    route.user_ctx = this;
    if (!accepted(httpd_register_uri_handler(http_, &route))) return false;
  }
  return true;
}

esp_err_t ManagementNetwork::handle(httpd_req_t* req) {
  return static_cast<ManagementNetwork*>(req->user_ctx)->route(req);
}

esp_err_t ManagementNetwork::route(httpd_req_t* req) {
  if (!headerIs(req, "Host", host_)) {
    error(req, "403 Forbidden", "HOST_REJECTED");
    return ESP_FAIL;
  }
  const auto bodyLimit = req->method == HTTP_POST && std::strcmp(req->uri, "/api/v1/inbox") == 0 ? 96u : 32u;
  if (req->content_len > bodyLimit || httpd_req_get_hdr_value_len(req, "Transfer-Encoding") != 0) {
    error(req, "413 Payload Too Large", "REQUEST_TOO_LARGE");
    return ESP_FAIL;
  }
  if (req->method == HTTP_GET && req->content_len == 0) return get(req);
  if (req->method == HTTP_GET) {
    error(req, "400 Bad Request", "UNEXPECTED_BODY");
    return ESP_FAIL;
  }
  if (!headerIs(req, "Origin", origin_)) {
    error(req, "403 Forbidden", "ORIGIN_REJECTED");
    return ESP_FAIL;
  }
  return mutate(req);
}

esp_err_t ManagementNetwork::get(httpd_req_t* req) {
  const char* uri = req->uri;
  if (std::strcmp(uri, "/") == 0)
    return send(req, "200 OK", "text/html; charset=utf-8", web::index, sizeof(web::index) - 1);
  if (std::strcmp(uri, "/style.css") == 0)
    return send(req, "200 OK", "text/css; charset=utf-8", web::style, sizeof(web::style) - 1);
  if (std::strcmp(uri, "/app.js") == 0)
    return send(req, "200 OK", "text/javascript; charset=utf-8", web::app, sizeof(web::app) - 1);
  if (std::strcmp(uri, "/apps.js") == 0)
    return send(req, "200 OK", "text/javascript; charset=utf-8", web::apps, sizeof(web::apps) - 1);
  if (std::strcmp(uri, "/api/v1/apps") == 0) return json(req, kApps);
  if (std::strcmp(uri, "/api/v1/radio") == 0 || std::strcmp(uri, "/api/v1/inbox") == 0) return appRead(req);
  if (std::strncmp(uri, "/api/v1/jobs/", 13) == 0) return jobs(req, false);
  if (std::strcmp(uri, "/api/v1/events") == 0) {
    Event events[8]{};
    portENTER_CRITICAL(&lock_);
    const unsigned count = eventCount_;
    for (unsigned i = 0; i < count; ++i) events[i] = events_[(nextEvent_ + 7 - i) % 8];
    portEXIT_CRITICAL(&lock_);
    char payload[1024] = "[";
    size_t used = 1;
    for (unsigned i = 0; i < count; ++i) {
      const auto& event = events[i];
      const int written = std::snprintf(payload + used, sizeof(payload) - used,
          R"(%s{"uptimeMs":%llu,"state":"%s","reason":"%s"})", i == 0 ? "" : ",",
          static_cast<unsigned long long>(event.atMs), serviceStateName(event.state), reasonCode(event.reason));
      if (written < 0 || static_cast<size_t>(written) >= sizeof(payload) - used - 1)
        return error(req, "500 Internal Server Error", "ENCODING_FAILED");
      used += written;
    }
    payload[used] = ']';
    payload[used + 1] = '\0';
    return json(req, payload);
  }

  portENTER_CRITICAL(&lock_);
  const auto data = snapshot_;
  const unsigned interval = pollingSeconds_;
  portEXIT_CRITICAL(&lock_);
  char response[1280]{};
  if (std::strcmp(uri, "/api/v1/system") == 0) {
    std::snprintf(response, sizeof(response),
                  R"({"chip":"ESP32-S3","firmwareVersion":"%s","buildId":"%s","cores":%u,"resetReason":%d})",
                  nexusVersion(), nexusBuildId(), nexusCoreCount(), nexusResetReason());
  } else if (std::strcmp(uri, "/api/v1/settings") == 0) {
    std::snprintf(response, sizeof(response),
                  R"({"ssid":"NEXUS-S3","channel":1,"clientLimit":1,"txPowerDbm":8.5,"pollSeconds":%u,"persistence":"current boot"})",
                  interval);
  } else if (std::strcmp(uri, "/api/v1/services") == 0) {
    std::snprintf(response, sizeof(response),
                  R"([{"id":"management","state":"%s","dependencies":[],"reason":"%s","internalHeadroomBytes":%u}])",
                  serviceStateName(data.service), reasonCode(data.reason), Guard::kManagementHeadroom);
  } else if (std::strcmp(uri, "/api/v1/guard") == 0) {
    char cutoff[24] = "null";
    if (data.leased) std::snprintf(cutoff, sizeof(cutoff), "%.2f", double(data.cutoff));
    std::snprintf(response, sizeof(response),
                  R"({"policyVersion":1,"state":"%s","reason":"%s","warning":"%s","cutoffC":%s,"heapReserveBytes":%u,"psramReserveBytes":%u})",
                  serviceStateName(data.service), reasonCode(data.reason), reasonCode(data.warning),
                  cutoff, kInternalReserve, kPsramReserve);
  } else if (std::strcmp(uri, "/api/v1/telemetry") == 0) {
    const auto& t = data.telemetry;
    char temperature[24] = "null";
    if (t.temperatureValid) std::snprintf(temperature, sizeof(temperature), "%.2f", double(t.temperatureC));
    std::snprintf(response, sizeof(response),
                  R"({"schemaVersion":1,"sequence":%lu,"uptimeMs":%llu,"cpu":{"frequencyMHz":%lu,"cores":%u,"utilizationPercent":null},"temperature":{"valueC":%s,"valid":%s,"source":"on-chip"},"flashBytes":%lu,"memory":{"internalFreeBytes":%lu,"internalLargestBlockBytes":%lu,"internalMinimumFreeBytes":%lu,"psramTotalBytes":%lu,"psramFreeBytes":%lu,"psramLargestBlockBytes":%lu,"psramMinimumFreeBytes":%lu},"storage":{"mounted":false,"usableBytes":null,"freeBytes":null},"guard":{"state":"%s","warning":"%s","reason":"%s"},"pollSeconds":%u})",
                  static_cast<unsigned long>(data.sequence), static_cast<unsigned long long>(t.uptimeMs),
                  static_cast<unsigned long>(t.cpuMHz), nexusCoreCount(), temperature,
                  t.temperatureValid ? "true" : "false", static_cast<unsigned long>(t.flashBytes),
                  static_cast<unsigned long>(t.internal.freeBytes), static_cast<unsigned long>(t.internal.largestBlockBytes),
                  static_cast<unsigned long>(t.internal.minimumFreeBytes), static_cast<unsigned long>(t.psramBytes),
                  static_cast<unsigned long>(t.psram.freeBytes), static_cast<unsigned long>(t.psram.largestBlockBytes),
                  static_cast<unsigned long>(t.psram.minimumFreeBytes), serviceStateName(data.service),
                  reasonCode(data.warning), reasonCode(data.reason), interval);
  } else {
    return error(req, "404 Not Found", "UNKNOWN_ROUTE");
  }
  return json(req, response);
}

bool ManagementNetwork::authenticated(httpd_req_t* req) {
  char auth[40]{};
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
      std::strlen(auth) != 39 || std::strncmp(auth, "Bearer ", 7) != 0) return false;
  const auto now = nowMs();
  portENTER_CRITICAL(&lock_);
  const bool valid = session_[0] != '\0' && now < sessionExpiresMs_ && matchesSecret(auth + 7, session_);
  portEXIT_CRITICAL(&lock_);
  mbedtls_platform_zeroize(auth, sizeof(auth));
  return valid;
}

esp_err_t ManagementNetwork::login(httpd_req_t* req) {
  const auto now = nowMs();
  portENTER_CRITICAL(&lock_);
  const bool limited = now < nextLoginMs_;
  portEXIT_CRITICAL(&lock_);
  if (limited) {
    error(req, "429 Too Many Requests", "TRY_LATER");
    return ESP_FAIL;
  }
  char input[33]{};
  if (!readBody(req, input, 32)) {
    error(req, "400 Bad Request", "INVALID_OPERATOR_CODE");
    return ESP_FAIL;
  }
  char token[33]{};
  randomHex(token);
  portENTER_CRITICAL(&lock_);
  nextLoginMs_ = now + 2000;
  const bool valid = operatorCode_[0] != '\0' && matchesSecret(input, operatorCode_);
  if (valid) {
    mbedtls_platform_zeroize(operatorCode_, sizeof(operatorCode_));
    std::memcpy(session_, token, sizeof(token));
    sessionExpiresMs_ = now + 300000;
  }
  portEXIT_CRITICAL(&lock_);
  mbedtls_platform_zeroize(input, sizeof(input));
  if (!valid) {
    mbedtls_platform_zeroize(token, sizeof(token));
    return error(req, "401 Unauthorized", "INVALID_OPERATOR_CODE");
  }
  char response[80]{};
  std::snprintf(response, sizeof(response), R"({"token":"%s","expiresInSeconds":300})", token);
  const auto result = json(req, response);
  mbedtls_platform_zeroize(token, sizeof(token));
  mbedtls_platform_zeroize(response, sizeof(response));
  return result;
}

esp_err_t ManagementNetwork::mutate(httpd_req_t* req) {
  const char* uri = req->uri;
  if (req->method == HTTP_POST && std::strcmp(uri, "/api/v1/auth/session") == 0) return login(req);
  if (!authenticated(req)) {
    error(req, "401 Unauthorized", "AUTH_REQUIRED");
    return req->content_len ? ESP_FAIL : ESP_OK;
  }
  if (req->method == HTTP_POST &&
      (std::strncmp(uri, "/api/v1/radio/", 14) == 0 || std::strcmp(uri, "/api/v1/inbox") == 0))
    return appWrite(req);
  if (req->method == HTTP_PATCH && std::strcmp(uri, "/api/v1/settings") == 0) {
    char value[2]{};
    if (!readBody(req, value, 1) || (value[0] != '2' && value[0] != '3' && value[0] != '5')) {
      error(req, "400 Bad Request", "INVALID_POLL_INTERVAL");
      return ESP_FAIL;
    }
    portENTER_CRITICAL(&lock_);
    pollingSeconds_ = static_cast<unsigned>(value[0] - '0');
    portEXIT_CRITICAL(&lock_);
    return json(req, R"({"applied":true,"persistence":"current boot"})");
  }
  if (req->content_len != 0) {
    error(req, "400 Bad Request", "UNEXPECTED_BODY");
    return ESP_FAIL;
  }
  if (req->method == HTTP_DELETE && std::strcmp(uri, "/api/v1/auth/session") == 0) {
    portENTER_CRITICAL(&lock_);
    mbedtls_platform_zeroize(session_, sizeof(session_));
    sessionExpiresMs_ = 0;
    portEXIT_CRITICAL(&lock_);
    return json(req, R"({"ended":true})");
  }
  if (req->method == HTTP_POST && std::strcmp(uri, "/api/v1/services/management/stop") == 0) {
    const auto now = nowMs();
    portENTER_CRITICAL(&lock_);
    const auto offer = job_.prepare();
    const auto remaining = job_.remainingMs(offer.id, now);
    portEXIT_CRITICAL(&lock_);
    char response[96]{};
    std::snprintf(response, sizeof(response), R"({"id":%lu,"accepted":true,"remainingMs":%lu})",
                  static_cast<unsigned long>(offer.id), static_cast<unsigned long>(remaining));
    const auto result = json(req, response, "202 Accepted");
    if (offer.created) {
      const auto responded = nowMs();
      portENTER_CRITICAL(&lock_);
      if (result == ESP_OK) job_.arm(offer.id, responded);
      else job_.cancel(offer.id, responded);
      portEXIT_CRITICAL(&lock_);
    }
    return result;
  }
  if (req->method == HTTP_POST && std::strncmp(uri, "/api/v1/jobs/", 13) == 0) return jobs(req, true);
  if (req->method == HTTP_POST && std::strcmp(uri, "/api/v1/services/management/start") == 0)
    return error(req, "409 Conflict", "USE_SERIAL_TO_START");
  return error(req, "404 Not Found", "UNKNOWN_ROUTE");
}

esp_err_t ManagementNetwork::appRead(httpd_req_t* req) {
  if (!authenticated(req)) return error(req, "401 Unauthorized", "AUTH_REQUIRED");
  char data[2048]{};
  size_t offset = 0;
  if (std::strcmp(req->uri, "/api/v1/radio") == 0) {
    const auto scan = radioApps_.result();
    offset = std::snprintf(data, sizeof(data),
        R"({"kind":"%s","state":"%s","reason":"%s","frames":%u,"mean":%u,"minimum":%u,"maximum":%u,"rows":[)",
        radioName(scan.kind), RadioApps::stateName(scan.state), scan.reason, scan.frames,
        scan.frames ? scan.amplitude / scan.frames : 0, scan.minAmplitude, scan.maxAmplitude);
    for (unsigned i = 0; i < scan.count; ++i) {
      const auto& row = scan.rows[i];
      char name[65]{};
      jsonAscii(row.name, name);
      const int written = std::snprintf(data + offset, sizeof(data) - offset,
          R"(%s{"name":"%s","rssi":%d,"channel":%u,"security":%u})",
          i == 0 ? "" : ",", name, row.rssi, row.channel, row.security);
      if (written < 0 || static_cast<size_t>(written) >= sizeof(data) - offset - 2)
        return error(req, "500 Internal Server Error", "ENCODING_FAILED");
      offset += written;
    }
    data[offset++] = ']';
    data[offset] = '}';
  } else {
    Message copy[8]{};
    portENTER_CRITICAL(&lock_);
    const auto total = messages_;
    std::memcpy(copy, inbox_, sizeof(copy));
    portEXIT_CRITICAL(&lock_);
    data[offset++] = '[';
    const auto count = total < 8 ? total : 8;
    for (unsigned i = 0; i < count; ++i) {
      const auto& item = copy[(total - count + i) % 8];
      char escaped[193]{};
      jsonAscii(item.text, escaped);
      const int written = std::snprintf(data + offset, sizeof(data) - offset,
          R"(%s{"id":%u,"text":"%s"})", i == 0 ? "" : ",", item.id, escaped);
      if (written < 0 || static_cast<size_t>(written) >= sizeof(data) - offset - 1)
        return error(req, "500 Internal Server Error", "ENCODING_FAILED");
      offset += written;
    }
    data[offset] = ']';
  }
  return json(req, data);
}

esp_err_t ManagementNetwork::appWrite(httpd_req_t* req) {
  if (std::strcmp(req->uri, "/api/v1/inbox") == 0) {
    char message[97]{};
    const auto length = req->content_len;
    if (length == 0 || !readBody(req, message, length)) {
      error(req, "400 Bad Request", "INVALID_MESSAGE");
      return ESP_FAIL;
    }
    for (size_t i = 0; i < length; ++i) {
      if (message[i] < 32 || message[i] > 126) return error(req, "400 Bad Request", "INVALID_MESSAGE");
    }
    portENTER_CRITICAL(&lock_);
    auto& slot = inbox_[messages_ % 8];
    slot.id = ++messages_;
    std::memcpy(slot.text, message, sizeof(message));
    portEXIT_CRITICAL(&lock_);
    return json(req, R"({"saved":true})", "201 Created");
  }
  if (req->content_len != 0) {
    error(req, "400 Bad Request", "UNEXPECTED_BODY");
    return ESP_FAIL;
  }
  const auto* operation = req->uri + 14;
  if (std::strcmp(operation, "cancel") == 0) {
    radioApps_.requestCancel();
    return json(req, R"({"queued":true})", "202 Accepted");
  }
  auto kind = RadioApps::Kind::Wifi;
  if (std::strcmp(operation, "ble") == 0) kind = RadioApps::Kind::Ble;
  else if (std::strcmp(operation, "csi") == 0) kind = RadioApps::Kind::Csi;
  else if (std::strcmp(operation, "wifi") != 0) return error(req, "404 Not Found", "UNKNOWN_ROUTE");
  if (const auto* reason = queueRadio(kind, nowMs())) {
    std::printf("RADIO REQUEST DENIED %s %s\n", radioName(kind), reason);
    return error(req, "409 Conflict", reason);
  }
  return json(req, R"({"queued":true})", "202 Accepted");
}

esp_err_t ManagementNetwork::jobs(httpd_req_t* req, bool cancel) {
  if (!authenticated(req)) return error(req, "401 Unauthorized", "AUTH_REQUIRED");
  const char* cursor = req->uri + 13;
  uint32_t id = 0;
  unsigned digits = 0;
  while (*cursor >= '0' && *cursor <= '9' && digits < 9) {
    id = id * 10 + static_cast<unsigned>(*cursor++ - '0');
    ++digits;
  }
  if (id == 0 || std::strcmp(cursor, cancel ? "/cancel" : "") != 0)
    return error(req, "404 Not Found", "UNKNOWN_JOB");
  const auto now = nowMs();
  portENTER_CRITICAL(&lock_);
  const auto previous = job_.state(id);
  const bool cancelled = cancel && job_.cancel(id, now);
  const auto state = job_.state(id);
  const auto remaining = job_.remainingMs(id, now);
  portEXIT_CRITICAL(&lock_);
  if (state == StopJob::State::None) return error(req, "404 Not Found", "UNKNOWN_JOB");
  if (cancel && !cancelled) return error(req, "409 Conflict", "STOP_ALREADY_RUNNING");
  if (cancelled && previous == StopJob::State::Queued)
    std::printf("MANAGEMENT STOP CANCELLED job=%lu\n", static_cast<unsigned long>(id));
  const char* label = state == StopJob::State::Cancelled ? "cancelled" :
                      state == StopJob::State::Stopping ? "stopping" : "queued";
  char response[96]{};
  std::snprintf(response, sizeof(response), R"({"id":%lu,"state":"%s","remainingMs":%lu})",
                static_cast<unsigned long>(id), label, static_cast<unsigned long>(remaining));
  return json(req, response);
}

}  // namespace nexus
