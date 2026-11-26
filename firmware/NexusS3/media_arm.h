#pragma once

#include <cstdint>

namespace nexus {
class MediaArm {
 public:
  void arm(uint64_t now) { deadline_ = now + 15000; }
  void clear() { deadline_ = 0; }
  bool consume(uint64_t now) {
    const bool allowed = now < deadline_;
    clear();
    return allowed;
  }

 private:
  uint64_t deadline_ = 0;
};
}  // namespace nexus
