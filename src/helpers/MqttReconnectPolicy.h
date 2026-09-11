#pragma once

// Reconnect policy for MQTT broker slots: ladder backoff, stability gate,
// circuit breaker and per-slot stagger.
//
// Motivation and shape are ported from agessaman/MeshCore (observer-firmware,
// MIT) MQTTConnectionPolicy.h, but this version is a dependency-free state
// machine so it can be exercised by the native test suite
// (test/test_mqtt_reconnect_policy). The reporter drives it from its own
// loop; esp-mqtt's internal auto-reconnect is disabled.
//
//   * failures move up a ladder of wait delays: 10s, 30s, 60s, 120s, 300s
//   * the ladder heals only after the connection stayed up for the stability
//     gate (2 minutes); flaps keep their accumulated backoff
//   * 3 consecutive failures at the top rung latch a circuit breaker: the
//     slot is then probed once every 30 minutes; a successful connect clears
//     the breaker (the ladder still needs the stability gate to reset)
//   * the first attempt of slot N is delayed by N * 3s so several brokers
//     never hammer the network stack at the same instant

#include <stdint.h>

namespace mqtt_reconnect {

// Ladder delays: 10s, 30s, 60s, 120s, then 300s at the cap.
constexpr uint32_t kLadderMs[] = {10000UL, 30000UL, 60000UL, 120000UL, 300000UL};
constexpr uint8_t kLadderRungs = 5;

// A connection must stay up this long before the ladder resets.
constexpr uint32_t kStableGateMs = 120000UL;

// Consecutive failures at the top rung before the breaker latches.
constexpr uint8_t kBreakerTripFailures = 3;

// Breaker probe interval.
constexpr uint32_t kBreakerProbeMs = 1800000UL;

// Spacing between successive slots' first connect attempt.
constexpr uint32_t kSlotStaggerMs = 3000UL;

constexpr uint32_t ladderDelayMs(uint8_t rung) {
  return kLadderMs[rung < kLadderRungs ? rung : (kLadderRungs - 1)];
}

constexpr uint32_t slotStaggerMs(uint8_t slot) {
  return (uint32_t)slot * kSlotStaggerMs;
}

struct State {
  uint8_t rung;              // current ladder rung (0..kLadderRungs-1)
  uint8_t cap_failures;      // consecutive failures while at the cap rung
  bool breaker;              // latched: probing on the slow timer
  bool connected;            // slot reports a live session
  bool attempt_pending;      // a next attempt is scheduled
  bool attempt_in_flight;    // an attempt is running right now
  bool session_live;         // a session was up (used to count drops once)
  uint32_t connected_since_ms;
  uint32_t next_attempt_ms;  // absolute, caller's ms tick domain
};

// First scheduling for a slot at boot/startup: stagger by slot index.
inline void seedInitial(State &s, uint32_t now_ms, uint8_t slot) {
  s.attempt_pending = true;
  s.next_attempt_ms = now_ms + slotStaggerMs(slot);
}

// Transport reported a live connection.
inline void onConnected(State &s, uint32_t now_ms) {
  s.connected = true;
  s.session_live = true;
  s.attempt_in_flight = false;
  s.attempt_pending = false;
  s.connected_since_ms = now_ms;
  s.breaker = false;
  s.cap_failures = 0;
}

// Periodic tick: heal the ladder once the connection has been stable for
// the gate window. Safe to call at any rate.
inline void onTick(State &s, uint32_t now_ms) {
  if (!s.connected) return;
  if ((uint32_t)(now_ms - s.connected_since_ms) >= kStableGateMs) {
    s.rung = 0;
    s.cap_failures = 0;
    s.breaker = false;
  }
}

// A connect attempt failed or an established session dropped. Advances the
// ladder exactly one step (callers must collapse the ERROR/DISCONNECTED pair
// into one call) and schedules the next attempt.
inline void onFailure(State &s, uint32_t now_ms) {
  s.connected = false;
  s.session_live = false;
  s.attempt_in_flight = false;
  uint32_t delay = ladderDelayMs(s.rung);
  if (s.rung < kLadderRungs - 1) {
    s.rung++;
  } else {
    if (s.cap_failures < UINT8_MAX) s.cap_failures++;
    if (s.cap_failures >= kBreakerTripFailures) s.breaker = true;
  }
  if (s.breaker) delay = kBreakerProbeMs;
  s.next_attempt_ms = now_ms + delay;
  s.attempt_pending = true;
}

// Ready for the next attempt? (Not connected, no attempt running, and the
// scheduled time has arrived or nothing is scheduled.)
inline bool attemptDue(const State &s, uint32_t now_ms) {
  if (s.connected || s.attempt_in_flight) return false;
  if (!s.attempt_pending) return true;
  return (int32_t)(now_ms - s.next_attempt_ms) >= 0;
}

// Milliseconds until the next scheduled attempt (0 if due / connected).
inline uint32_t nextWaitMs(const State &s, uint32_t now_ms) {
  if (s.connected || s.attempt_in_flight || !s.attempt_pending) return 0;
  int32_t remaining = (int32_t)(s.next_attempt_ms - now_ms);
  return remaining > 0 ? (uint32_t)remaining : 0;
}

// Full reset: used by `mqtt reconnect` and configuration resets. The next
// attempt becomes due immediately.
inline void reset(State &s) {
  s = State{};
}

}  // namespace mqtt_reconnect
