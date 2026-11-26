#include "field_store.h"

#include <cstdio>
#include <cstring>
#include "bootloader_random.h"
#include "esp_random.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace nexus {
namespace {

constexpr char kNamespace[] = "nexus_field";
constexpr char kKeyWifi[] = "ap_pass";
constexpr char kKeyPass[] = "op_ver";

bool deriveKey(const char* input, size_t length, const uint8_t (&salt)[kFieldSaltBytes],
               uint8_t (&output)[kFieldHashBytes]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(
             MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(input), length,
             salt, kFieldSaltBytes, kFieldPbkdf2Iterations, kFieldHashBytes, output) == 0;
}

}  // namespace

bool FieldStore::init() {
  bootloader_random_enable();
  esp_fill_random(bootEntropy_, sizeof(bootEntropy_));
  bootloader_random_disable();

  const esp_err_t status = nvs_flash_init();
  if (status != ESP_OK) {
    std::printf("FIELD_NVS UNAVAILABLE %s\n", esp_err_to_name(status));
    nvsReady_ = false;
    return false;
  }
  nvsReady_ = true;

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return true;  // Namespace does not exist yet; clean ephemeral default.
  }

  size_t wifiLen = sizeof(wifiPassword_);
  char loadedWifi[64]{};
  if (nvs_get_str(handle, kKeyWifi, loadedWifi, &wifiLen) == ESP_OK) {
    const size_t actualLen = std::strlen(loadedWifi);
    if (validFieldWifiPassword(loadedWifi, actualLen)) {
      std::memcpy(wifiPassword_, loadedWifi, actualLen + 1);
      hasWifi_ = true;
    }
  }
  mbedtls_platform_zeroize(loadedWifi, sizeof(loadedWifi));

  uint8_t verifier[kFieldVerifierBytes]{};
  size_t verifierLen = sizeof(verifier);
  if (nvs_get_blob(handle, kKeyPass, verifier, &verifierLen) == ESP_OK &&
      verifierLen == kFieldVerifierBytes) {
    std::memcpy(salt_, verifier, kFieldSaltBytes);
    std::memcpy(digest_, verifier + kFieldSaltBytes, kFieldHashBytes);
    hasPass_ = true;
  }
  mbedtls_platform_zeroize(verifier, sizeof(verifier));

  nvs_close(handle);
  return true;
}

bool FieldStore::copyWifiPassword(char (&output)[64]) const {
  if (!hasWifi_) return false;
  std::memcpy(output, wifiPassword_, sizeof(output));
  return true;
}

bool FieldStore::verifyOperatorPassphrase(const char* input, size_t length) const {
  if (!hasPass_ || !validFieldOperatorPassphrase(input, length)) return false;
  uint8_t derived[kFieldHashBytes]{};
  const bool ok = deriveKey(input, length, salt_, derived) &&
                  constantTimeEqual(derived, digest_, kFieldHashBytes);
  mbedtls_platform_zeroize(derived, sizeof(derived));
  return ok;
}

bool FieldStore::saveWifiPassword(const char* input, size_t length) {
  if (!nvsReady_ || !validFieldWifiPassword(input, length)) return false;
  char clean[64]{};
  std::memcpy(clean, input, length);
  clean[length] = '\0';

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    mbedtls_platform_zeroize(clean, sizeof(clean));
    return false;
  }
  const bool saved = nvs_set_str(handle, kKeyWifi, clean) == ESP_OK &&
                     nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (saved) {
    std::memcpy(wifiPassword_, clean, sizeof(wifiPassword_));
    hasWifi_ = true;
  }
  mbedtls_platform_zeroize(clean, sizeof(clean));
  return saved;
}

bool FieldStore::saveOperatorPassphrase(const char* input, size_t length) {
  if (!nvsReady_ || !validFieldOperatorPassphrase(input, length)) return false;
  uint8_t newSalt[kFieldSaltBytes]{};
  uint8_t newDigest[kFieldHashBytes]{};
  uint8_t verifier[kFieldVerifierBytes]{};

  esp_fill_random(newSalt, sizeof(newSalt));
  ++saltCounter_;
  for (size_t i = 0; i < kFieldSaltBytes; ++i) {
    newSalt[i] ^= bootEntropy_[i] ^ static_cast<uint8_t>(saltCounter_ >> ((i % 4) * 8));
  }

  if (!deriveKey(input, length, newSalt, newDigest)) {
    mbedtls_platform_zeroize(newSalt, sizeof(newSalt));
    mbedtls_platform_zeroize(newDigest, sizeof(newDigest));
    return false;
  }
  std::memcpy(verifier, newSalt, kFieldSaltBytes);
  std::memcpy(verifier + kFieldSaltBytes, newDigest, kFieldHashBytes);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    mbedtls_platform_zeroize(newSalt, sizeof(newSalt));
    mbedtls_platform_zeroize(newDigest, sizeof(newDigest));
    mbedtls_platform_zeroize(verifier, sizeof(verifier));
    return false;
  }
  const bool saved = nvs_set_blob(handle, kKeyPass, verifier, sizeof(verifier)) == ESP_OK &&
                     nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (saved) {
    std::memcpy(salt_, newSalt, sizeof(salt_));
    std::memcpy(digest_, newDigest, sizeof(digest_));
    hasPass_ = true;
  }
  mbedtls_platform_zeroize(newSalt, sizeof(newSalt));
  mbedtls_platform_zeroize(newDigest, sizeof(newDigest));
  mbedtls_platform_zeroize(verifier, sizeof(verifier));
  return saved;
}

bool FieldStore::clear() {
  hasWifi_ = false;
  hasPass_ = false;
  mbedtls_platform_zeroize(wifiPassword_, sizeof(wifiPassword_));
  mbedtls_platform_zeroize(salt_, sizeof(salt_));
  mbedtls_platform_zeroize(digest_, sizeof(digest_));
  if (!nvsReady_) return true;
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t e1 = nvs_erase_key(handle, kKeyWifi);
  const esp_err_t e2 = nvs_erase_key(handle, kKeyPass);
  const bool ok = (e1 == ESP_OK || e1 == ESP_ERR_NVS_NOT_FOUND) &&
                  (e2 == ESP_OK || e2 == ESP_ERR_NVS_NOT_FOUND) &&
                  nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace nexus
