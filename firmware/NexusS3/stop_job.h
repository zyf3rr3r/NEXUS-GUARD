#pragma once

#include <cstdint>

namespace nexus {

// The service owner executes the stop. The HTTP task may queue it or cancel before takeover.
class StopJob {
 public:
  enum class State { None, Queued, Stopping, Cancelled };
  static constexpr uint32_t kCancelWindowMs = 15000;

  struct Offer {
    uint32_t id;
    bool created;
  };

  Offer prepare() {
    if (state_ != State::None && state_ != State::Cancelled) return {id_, false};
    id_ = nextId_++;
    state_ = State::Queued;
    dueMs_ = 0;
    return {id_, true};
  }

  void arm(uint32_t id, uint64_t nowMs) {
    if (state(id) == State::Queued && dueMs_ == 0) dueMs_ = nowMs + kCancelWindowMs;
  }

  uint32_t remainingMs(uint32_t id, uint64_t nowMs) const {
    if (state(id) != State::Queued) return 0;
    if (dueMs_ == 0) return kCancelWindowMs;
    return nowMs < dueMs_ ? static_cast<uint32_t>(dueMs_ - nowMs) : 0;
  }

  bool take(uint64_t nowMs) {
    if (state_ != State::Queued || dueMs_ == 0 || nowMs < dueMs_) return false;
    state_ = State::Stopping;
    return true;
  }

  bool cancel(uint32_t id, uint64_t nowMs) {
    if (state(id) == State::Cancelled) return true;
    if (remainingMs(id, nowMs) == 0) return false;
    state_ = State::Cancelled;
    return true;
  }

  State state(uint32_t id) const { return id != 0 && id == id_ ? state_ : State::None; }
  void clear() { state_ = State::None; }

 private:
  uint32_t nextId_ = 1;
  uint32_t id_ = 0;
  uint64_t dueMs_ = 0;
  State state_ = State::None;
};

}  // namespace nexus
