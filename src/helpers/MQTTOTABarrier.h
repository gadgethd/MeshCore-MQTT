#pragma once

#include <atomic>
#include <stdint.h>

// Dependency-free OTA upload gate shared by the ESP32 upload path and native
// tests. Flash I/O is possible only after the same upload attempt observes a
// verified clean MQTT stop. Abort and failed-stop states are terminal until a
// new attempt explicitly resets the barrier.
namespace MQTTOTA {

enum class State : uint8_t {
  Idle = 0,
  WaitingForStop,
  ReadyToFlash,
  Flashing,
  Aborted,
};

class Barrier {
 public:
  State state() const { return _state.load(std::memory_order_acquire); }

  void reset() { _state.store(State::Idle, std::memory_order_release); }

  bool requestStop() {
    State expected = State::Idle;
    return _state.compare_exchange_strong(
        expected, State::WaitingForStop,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }

  bool onStopComplete(bool clean) {
    State expected = State::WaitingForStop;
    const State next = clean ? State::ReadyToFlash : State::Aborted;
    return _state.compare_exchange_strong(
        expected, next, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // Called immediately before every Update.begin/write/end operation. The
  // first permitted call opens the flash phase; subsequent calls remain
  // permitted only while the lifecycle's independent clean-stop gate agrees.
  bool allowFlashIO(bool lifecycle_allows_flash) {
    if (!lifecycle_allows_flash) {
      abort();
      return false;
    }

    State current = _state.load(std::memory_order_acquire);
    if (current == State::ReadyToFlash) {
      _state.compare_exchange_strong(
          current, State::Flashing,
          std::memory_order_acq_rel, std::memory_order_acquire);
      current = _state.load(std::memory_order_acquire);
    }
    return current == State::Flashing;
  }

  void abort() { _state.store(State::Aborted, std::memory_order_release); }

  bool flashStarted() const { return state() == State::Flashing; }

 private:
  std::atomic<State> _state{State::Idle};
};

}  // namespace MQTTOTA
