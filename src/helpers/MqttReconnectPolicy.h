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
//   * every retry for slot N adds N * 3s, and a shared 15s guard permits only
//     one broker attempt per maintenance pass
//   * attempts that never receive a terminal callback are reconciled after a
//     bounded deadline instead of remaining in flight forever

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

// Spacing between successive slots' connect attempts.
constexpr uint32_t kSlotStaggerMs = 3000UL;

// Minimum spacing between attempts made by different broker slots.
constexpr uint32_t kReconnectGuardMs = 15000UL;

// Upper bound for an attempt whose CONNECTED/ERROR/DISCONNECTED callback was
// lost. This is deliberately longer than the SDK's nominal 10s network
// timeout so normal asynchronous cleanup has time to report its terminal
// event before the owner loop reconciles it.
constexpr uint32_t kAttemptTimeoutMs = 60000UL;

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
  uint32_t attempt_started_ms;
  uint32_t next_attempt_ms;  // absolute, caller's ms tick domain
};

struct AttemptGuard {
  bool armed;
  uint32_t last_attempt_ms;
};

inline uint32_t nextGeneration(uint32_t current) {
  current++;
  return current == 0 ? 1 : current;
}

inline bool generationMatches(uint32_t active, uint32_t event) {
  return active != 0 && active == event;
}

// First scheduling for a slot at boot/startup: stagger by slot index.
inline void seedInitial(State &s, uint32_t now_ms, uint8_t slot) {
  s.attempt_pending = true;
  s.next_attempt_ms = now_ms + slotStaggerMs(slot);
}

inline void onAttemptStarted(State &s, uint32_t now_ms) {
  s.attempt_pending = false;
  s.attempt_in_flight = true;
  s.attempt_started_ms = now_ms;
}

// Transport reported a live connection.
// Returns false for duplicate CONNECTED callbacks.
inline bool onConnected(State &s, uint32_t now_ms) {
  if (s.connected) return false;
  s.connected = true;
  s.session_live = true;
  s.attempt_in_flight = false;
  s.attempt_pending = false;
  s.connected_since_ms = now_ms;
  s.breaker = false;
  // Keep cap_failures until the stability gate. Clearing it on CONNACK lets
  // an accept-then-drop broker evade the breaker forever.
  return true;
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
inline void onFailure(State &s, uint32_t now_ms, uint8_t slot) {
  s.connected = false;
  s.session_live = false;
  s.attempt_in_flight = false;
  uint32_t delay = ladderDelayMs(s.rung) + slotStaggerMs(slot);
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

// ERROR and DISCONNECTED are two observations of the same terminal outcome.
// Only the first one for an active attempt/session advances the policy.
inline bool onTerminalEvent(State &s, uint32_t now_ms, uint8_t slot) {
  if (!s.attempt_in_flight && !s.session_live) return false;
  onFailure(s, now_ms, slot);
  return true;
}

inline bool reconcileAttemptTimeout(State &s, uint32_t now_ms, uint8_t slot) {
  if (!s.attempt_in_flight ||
      (uint32_t)(now_ms - s.attempt_started_ms) < kAttemptTimeoutMs) {
    return false;
  }
  onFailure(s, now_ms, slot);
  return true;
}

// A restored WiFi link is a new failure domain. Re-probe promptly, retaining
// top-rung/breaker accounting until a connection passes the stability gate.
inline void onWifiRecovered(State &s, uint32_t now_ms, uint8_t slot) {
  if (s.connected) return;
  if (s.rung > 1) s.rung = 1;
  s.session_live = false;
  s.attempt_in_flight = false;
  s.attempt_pending = true;
  s.next_attempt_ms = now_ms + slotStaggerMs(slot);
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

inline bool guardAllowsAttempt(const AttemptGuard &guard, uint32_t now_ms,
                               bool attempted_this_pass) {
  if (attempted_this_pass) return false;
  return !guard.armed ||
         (uint32_t)(now_ms - guard.last_attempt_ms) >= kReconnectGuardMs;
}

inline void noteAttempt(AttemptGuard &guard, uint32_t now_ms) {
  guard.armed = true;
  guard.last_attempt_ms = now_ms;
}

inline void resetGuard(AttemptGuard &guard) {
  guard = AttemptGuard{};
}

// Full reset: used by `mqtt reconnect` and configuration resets. The next
// attempt becomes due immediately.
inline void reset(State &s) {
  s = State{};
}

}  // namespace mqtt_reconnect
