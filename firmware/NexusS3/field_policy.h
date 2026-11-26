#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nexus {

constexpr size_t kMinFieldWifiLen = 12;
constexpr size_t kMaxFieldWifiLen = 63;
constexpr size_t kMinFieldPassLen = 8;
constexpr size_t kMaxFieldPassLen = 32;
constexpr size_t kFieldSaltBytes = 16;
constexpr size_t kFieldHashBytes = 32;
constexpr size_t kFieldVerifierBytes = kFieldSaltBytes + kFieldHashBytes;
constexpr unsigned kFieldPbkdf2Iterations = 10000;

inline bool isPrintableAscii(const char* text, size_t length) {
  if (!text || length == 0) return false;
  bool hasNonSpace = false;
  for (size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c > 0x7E) return false;
    if (c != ' ') hasNonSpace = true;
  }
  return hasNonSpace;
}

inline bool validFieldWifiPassword(const char* text, size_t length) {
  return length >= kMinFieldWifiLen && length <= kMaxFieldWifiLen && isPrintableAscii(text, length);
}

inline bool validFieldOperatorPassphrase(const char* text, size_t length) {
  return length >= kMinFieldPassLen && length <= kMaxFieldPassLen && isPrintableAscii(text, length);
}

inline bool validOneTimeHexCode(const char* text, size_t length) {
  if (!text || length != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

inline bool constantTimeEqual(const uint8_t* first, const uint8_t* second, size_t length) {
  if (!first || !second) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < length; ++i) diff |= static_cast<unsigned>(first[i] ^ second[i]);
  return diff == 0;
}

class FieldButton {
 public:
  static constexpr uint64_t kDebounceMs = 50;
  static constexpr uint64_t kStartHoldMs = 1500;

  enum class Action { None, Start, Stop };

  Action poll(bool rawPressed, uint64_t nowMs, bool running) {
    if (!armed_) {
      if (!rawPressed) {
        armed_ = true;
        stablePressed_ = false;
        rawState_ = false;
        rawChangedMs_ = nowMs;
      }
      return Action::None;
    }

    if (rawPressed != rawState_) {
      rawState_ = rawPressed;
      rawChangedMs_ = nowMs;
    }

    if (rawState_ != stablePressed_ && nowMs >= rawChangedMs_ && nowMs - rawChangedMs_ >= kDebounceMs) {
      stablePressed_ = rawState_;
      if (stablePressed_) {
        pressStartedMs_ = nowMs;
        triggered_ = false;
      } else {
        const bool shouldStop = running && !triggered_;
        triggered_ = false;
        if (shouldStop) return Action::Stop;
      }
    }

    if (stablePressed_ && !triggered_ && nowMs >= pressStartedMs_ &&
        nowMs - pressStartedMs_ >= kStartHoldMs) {
      triggered_ = true;
      return running ? Action::Stop : Action::Start;
    }

    return Action::None;
  }

  bool holdingToStart(bool running) const {
    return armed_ && stablePressed_ && !triggered_ && !running;
  }

 private:
  bool armed_ = false;
  bool rawState_ = false;
  bool stablePressed_ = false;
  bool triggered_ = false;
  uint64_t rawChangedMs_ = 0;
  uint64_t pressStartedMs_ = 0;
};

struct FieldCommand {
  enum class Kind { Invalid, Status, SetWifi, SetPass, Clear };
  Kind kind = Kind::Invalid;
  const char* arg = nullptr;
  size_t argLen = 0;
};

inline FieldCommand parseFieldCommand(const char* line, size_t length) {
  if (!line || length == 0 || line[0] != ':') return {};
  if (length == 6 && std::memcmp(line, ":field", 6) == 0) return {FieldCommand::Kind::Status, nullptr, 0};
  if (length == 6 && std::memcmp(line, ":clear", 6) == 0) return {FieldCommand::Kind::Clear, nullptr, 0};
  if (length > 6 && std::memcmp(line, ":wifi ", 6) == 0) {
    const char* arg = line + 6;
    const size_t argLen = length - 6;
    if (validFieldWifiPassword(arg, argLen)) return {FieldCommand::Kind::SetWifi, arg, argLen};
    return {};
  }
  if (length > 6 && std::memcmp(line, ":pass ", 6) == 0) {
    const char* arg = line + 6;
    const size_t argLen = length - 6;
    if (validFieldOperatorPassphrase(arg, argLen)) return {FieldCommand::Kind::SetPass, arg, argLen};
    return {};
  }
  return {};
}

}  // namespace nexus
