#pragma once

#include "field_policy.h"

#include <cstddef>
#include <cstdint>

namespace nexus {

class FieldStore {
 public:
  // Initializes NVS without ever calling nvs_flash_erase(). Degrades safely if unavailable.
  bool init();

  bool nvsReady() const { return nvsReady_; }
  bool hasWifi() const { return hasWifi_; }
  bool hasPass() const { return hasPass_; }
  bool active() const { return hasWifi_ && hasPass_; }

  bool copyWifiPassword(char (&output)[64]) const;
  bool verifyOperatorPassphrase(const char* input, size_t length) const;

  bool saveWifiPassword(const char* input, size_t length);
  bool saveOperatorPassphrase(const char* input, size_t length);
  bool clear();

 private:
  bool nvsReady_ = false;
  bool hasWifi_ = false;
  bool hasPass_ = false;
  uint32_t saltCounter_ = 0;
  uint8_t bootEntropy_[kFieldSaltBytes]{};
  char wifiPassword_[64]{};
  uint8_t salt_[kFieldSaltBytes]{};
  uint8_t digest_[kFieldHashBytes]{};
};

}  // namespace nexus
