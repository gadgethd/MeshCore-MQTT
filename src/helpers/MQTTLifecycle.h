#pragma once

#include <stdint.h>

// Fork-owned, dependency-free MQTT reporter lifecycle state machine and the
// narrow dependency seam used to drive it deterministically in host tests.
//
// Adapted from agessaman/MeshCore@observer-firmware src/helpers/MQTTLifecycle.h
// (MIT licence, same upstream project) for our single-task, loop-driven,
// multi-slot MqttReporter. Kept intentionally pure (no Arduino, FreeRTOS,
// WiFi, ESP-MQTT) so the start/stop/restart contract can be exercised with a
// fake clock and a recording Ops double, the same way MqttReconnectPolicy.h
// is tested.
//
// MqttReporter supplies the production Ops adapter. The same dependency seam
// remains usable from host tests so ownership and flash-gate behavior stay
// deterministic without Arduino, FreeRTOS, WiFi, or ESP-MQTT.
//
// Behavior source: every transition and invariant encoded here is derived from
// the failure modes we have actually observed in the field and on the bench
// (teardown-during-OTA = the heap-panic path; abrupt destroy from loop;
// reconnect churn during reconfig) — not guessed. Values that require
// on-hardware characterisation (the concrete stop timeout, exact TLS teardown
// timing) are injectable parameters, not baked-in constants.
//
// StopRequested: a stop has been requested and delivered through the ownership
//   channel, but the reporter has not yet begun its ordered shutdown.
// Stopping:      the reporter is performing its ordered per-slot shutdown.
//   A StopBegan signal is optional; a reporter may ack directly from
//   StopRequested if it does not report the intermediate step.
namespace MQTTLifecycle {

enum class State : uint8_t {
  Stopped = 0,
  Starting,
  Running,
  StopRequested,
  Stopping,
  // The stop deadline expired without an acknowledgement. The reporter may
  // still own clients or be inside the SDK, so nothing it can reach may be
  // released and no new lifecycle may start. A late acknowledgement is the
  // only in-boot transition out of this state (post-F01 ownership rule).
  StopUnproven,
};

// Events driven either by the owner (loop task) or by the reporter itself
// reporting its own progress. StopTimedOut is synthesized by the Coordinator
// when a stop is not acknowledged within the bounded timeout (the reviewed
// fallback: never hang the loop or the OTA barrier indefinitely).
enum class Event : uint8_t {
  StartRequested = 0,  // owner asked the reporter to start
  StartCompleted,      // reporter signalled init complete
  StartFailed,         // init failed (partial-init rollback)
  StopRequested,       // owner (or OTA barrier) asked the reporter to stop
  StopBegan,           // reporter began its ordered shutdown (optional)
  StopAcknowledged,    // reporter signalled ordered shutdown complete
  StopTimedOut,        // bounded timeout expired with no ack (fallback)
};

// Side effects a transition asks the caller to perform. Naming WHO does WHAT
// keeps the production wiring and the host fakes on one contract:
//  - start_clients:     create/connect the clients for enabled slots.
//  - deliver_stop:      deliver the stop request (cessation of new work).
//  - release_resources: clients, outbox buffers and allocations may now be
//                       freed. Fires only after an acknowledged stop or an
//                       init-failure rollback - never on an unproven timeout.
//  - ota_release:       the OTA barrier's completion acknowledgement is now
//                       available (a stop reached a terminal state).
struct Effects {
  bool start_clients = false;
  bool deliver_stop = false;
  bool release_resources = false;
  bool ota_release = false;
};

struct Result {
  State next;
  Effects effects;
  bool accepted;  // false => the event was a no-op in this state (idempotency)
};

// Unsigned subtraction is the standard millis() idiom and stays correct across
// a single 32-bit rollover (mirrors mqtt_reconnect timing).
inline uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }

// Pure transition function. For a rejected/no-op event the result reports the
// unchanged state, no effects, and accepted == false.
inline Result apply(State s, Event e) {
  Result r{s, Effects{}, false};
  switch (s) {
    case State::Stopped:
      // Only a start is meaningful. A stop while already stopped is a no-op
      // (idempotent stop). StartFailed/StopAck cannot occur here.
      if (e == Event::StartRequested) {
        r.next = State::Starting;
        r.effects.start_clients = true;
        r.accepted = true;
      }
      break;

    case State::Starting:
      switch (e) {
        case Event::StartCompleted:
          r.next = State::Running;
          r.accepted = true;
          break;
        case Event::StartFailed:
          // Partial-init rollback: release only what the attempt acquired.
          r.next = State::Stopped;
          r.effects.release_resources = true;
          r.accepted = true;
          break;
        case Event::StopRequested:
          // Stop before full initialization: accept it and let the reporter ack.
          r.next = State::StopRequested;
          r.effects.deliver_stop = true;
          r.accepted = true;
          break;
        default:
          break;
      }
      break;

    case State::Running:
      if (e == Event::StopRequested) {
        r.next = State::StopRequested;
        r.effects.deliver_stop = true;
        r.accepted = true;
      }
      // A duplicate StartRequested/StartCompleted while Running is a no-op.
      break;

    case State::StopRequested:
      switch (e) {
        case Event::StopBegan:
          r.next = State::Stopping;
          r.accepted = true;
          break;
        case Event::StopAcknowledged:
          r.next = State::Stopped;
          r.effects.release_resources = true;
          r.effects.ota_release = true;
          r.accepted = true;
          break;
        case Event::StopTimedOut:
          r.next = State::StopUnproven;
          r.effects.ota_release = true;  // wake the barrier so it can abort
          r.accepted = true;
          break;
        default:
          // Duplicate StopRequested is a no-op (idempotent stop).
          break;
      }
      break;

    case State::Stopping:
      switch (e) {
        case Event::StopAcknowledged:
          r.next = State::Stopped;
          r.effects.release_resources = true;
          r.effects.ota_release = true;
          r.accepted = true;
          break;
        case Event::StopTimedOut:
          r.next = State::StopUnproven;
          r.effects.ota_release = true;
          r.accepted = true;
          break;
        default:
          break;
      }
      break;

    case State::StopUnproven:
      // The deadline is an availability bound, not proof that teardown failed.
      // A late acknowledgement is safe to honor because the reporter emits it
      // only after its ordered teardown returns. It releases withheld resources
      // and permits restart, but the dirty flash latch remains set.
      if (e == Event::StopAcknowledged) {
        r.next = State::Stopped;
        r.effects.release_resources = true;
        r.accepted = true;
      }
      break;
  }
  return r;
}

// New connects/publishes/retries/reconfigurations are permitted only while the
// reporter is actively running (or still coming up). Once a stop is requested,
// all new work must cease (cessation of new connects, publishes, retries, and
// reconfigurations).
inline bool acceptsNewWork(State s) {
  return s == State::Starting || s == State::Running;
}

// A late/stale client callback may touch owned state ONLY before the owner has
// freed it. After the terminal Stopped state the clients, buffers and
// allocations may be gone, so a callback arriving then must be a no-op.
// (Callbacks that fire during StopRequested/Stopping run before
// release_resources and are still safe.)
inline bool mayTouchOwnedState(State s) { return s != State::Stopped; }

// A restart is safe only from a completed stop.
inline bool mayRestart(State s) { return s == State::Stopped; }

inline bool isStopUnproven(State s) { return s == State::StopUnproven; }

inline bool isStopInProgress(State s) {
  return s == State::StopRequested || s == State::Stopping;
}

// Conservative post-audit stop window. The reference hardware evidence found
// roughly 5-6 seconds per sequential WSS slot; its corrected policy uses a
// five-second base plus eight seconds for every owned slot, clamped to at least
// one slot. Count allocated/started/connecting clients, not only connected
// sockets. Re-characterise this bound on our hardware before an OTA release.
constexpr uint32_t MQTT_STOP_BASE_TIMEOUT_MS = 5000u;
constexpr uint32_t MQTT_STOP_PER_SLOT_TIMEOUT_MS = 8000u;
static_assert(MQTT_STOP_BASE_TIMEOUT_MS >= 5000u,
              "MQTT stop base timeout must retain the audited safety margin");
static_assert(MQTT_STOP_PER_SLOT_TIMEOUT_MS >= 8000u,
              "MQTT stop per-slot timeout must retain the audited safety margin");

constexpr uint32_t suggestedStopTimeoutMs(uint8_t owned_slots) {
  return MQTT_STOP_BASE_TIMEOUT_MS +
         MQTT_STOP_PER_SLOT_TIMEOUT_MS * (uint32_t)(owned_slots == 0 ? 1 : owned_slots);
}

inline const char* stateName(State s) {
  switch (s) {
    case State::Stopped:       return "Stopped";
    case State::Starting:      return "Starting";
    case State::Running:       return "Running";
    case State::StopRequested: return "StopRequested";
    case State::Stopping:      return "Stopping";
    case State::StopUnproven:  return "StopUnproven";
  }
  return "?";
}

inline const char* eventName(Event e) {
  switch (e) {
    case Event::StartRequested:   return "StartRequested";
    case Event::StartCompleted:   return "StartCompleted";
    case Event::StartFailed:      return "StartFailed";
    case Event::StopRequested:    return "StopRequested";
    case Event::StopBegan:        return "StopBegan";
    case Event::StopAcknowledged: return "StopAcknowledged";
    case Event::StopTimedOut:     return "StopTimedOut";
  }
  return "?";
}

// Narrow dependency seam. These are the only dependencies the lifecycle needs
// to be driven deterministically. Production wiring implements this over the
// reporter and ESP-MQTT; host tests implement it as a recording double with a
// settable clock.
//
//  - Clock/timer                      -> nowMs()
//  - Slot start/stop/ack/timeout      -> startClients()/deliverStop() + the
//                                        onStarted()/onStopped() callbacks
//                                        into Coordinator, and tick() for
//                                        the timeout
//  - Runtime allocator + queue        -> releaseResources()
//  - MQTT client connect/disconnect   -> modeled by the mayTouchOwnedState()
//                                        guard (a callback decides whether it
//                                        may touch owned state)
//  - OTA/deep-sleep barrier           -> onStopComplete(clean) + mayBeginFlash()
struct Ops {
  virtual ~Ops() = default;
  virtual uint32_t nowMs() = 0;          // monotonic ms (millis())
  virtual void startClients() = 0;       // create/connect enabled slots
  virtual void deliverStop() = 0;        // signal stop; all new work ceases
  virtual void releaseResources() = 0;   // free clients/buffers/allocations
  // Unblock the OTA barrier's waiter. clean == true after a StopAcknowledged;
  // clean == false after a StopTimedOut, so OTA abort rather than flash under
  // uncertain ownership.
  virtual void onStopComplete(bool clean) = 0;
};

// Drives the state machine against injected Ops and hosts the bounded stop
// timeout. Idempotent start/stop; safe restart after a completed stop.
class Coordinator {
 public:
  // stop_timeout_ms bounds how long a requested stop may run before ownership
  // becomes unproven and the flash barrier aborts. It is injected and may be
  // updated per stop via setStopTimeoutMs() - size it to every currently owned
  // client before requestStop() (see suggestedStopTimeoutMs()).
  Coordinator(Ops& ops, uint32_t stop_timeout_ms)
      : _ops(ops), _stop_timeout_ms(stop_timeout_ms) {}

  State state() const { return _state; }
  bool stopTimedOut() const { return _stop_timed_out; }

  bool acceptsNewWork() const { return MQTTLifecycle::acceptsNewWork(_state); }
  bool mayTouchOwnedState() const {
    return MQTTLifecycle::mayTouchOwnedState(_state);
  }
  bool isStopInProgress() const {
    return MQTTLifecycle::isStopInProgress(_state);
  }
  // A restart is safe only from a proven stop. StopUnproven retains ownership
  // and refuses a second start until the original teardown acknowledges late.
  bool mayRestart() const { return MQTTLifecycle::mayRestart(_state); }
  bool isStopUnproven() const { return MQTTLifecycle::isStopUnproven(_state); }
  // OTA erase/write (and deep-sleep entry) is permitted only after this
  // coordinator has observed a CLEAN stop acknowledgement. The fresh Stopped
  // state is deliberately fail-closed: merely having no active lifecycle is
  // not proof that the production owner completed teardown.
  bool mayBeginFlash() const {
    return _state == State::Stopped && _clean_stop_acknowledged;
  }

  // Update the stop-timeout bound. Call before requestStop() to size the
  // window to the current slot count; the value is read by tick() against
  // _stop_request_ms, which requestStop() arms afterwards.
  void setStopTimeoutMs(uint32_t ms) { _stop_timeout_ms = ms; }
  uint32_t stopTimeoutMs() const { return _stop_timeout_ms; }

  bool requestStart() { return dispatch(Event::StartRequested); }
  bool requestStop() { return dispatch(Event::StopRequested); }
  bool onStarted() { return dispatch(Event::StartCompleted); }
  bool onStartFailed() { return dispatch(Event::StartFailed); }
  bool onStopBegan() { return dispatch(Event::StopBegan); }
  bool onStopped() { return dispatch(Event::StopAcknowledged); }

  // Call periodically from the owner. Fires the reviewed timeout fallback if a
  // requested stop has not been acknowledged within stop_timeout_ms.
  void tick() {
    if (MQTTLifecycle::isStopInProgress(_state) &&
        elapsedMs(_ops.nowMs(), _stop_request_ms) >= _stop_timeout_ms) {
      dispatch(Event::StopTimedOut);
    }
  }

 private:
  bool dispatch(Event e) {
    const Result r = apply(_state, e);
    if (!r.accepted) return false;

    _state = r.next;
    if (e == Event::StartRequested) {
      _stop_timed_out = false;  // a fresh start clears the dirty-stop latch
      _clean_stop_acknowledged = false;
    } else if (e == Event::StopRequested) {
      _stop_request_ms = _ops.nowMs();  // arm the timeout window
      _clean_stop_acknowledged = false;
    } else if (e == Event::StopTimedOut) {
      _stop_timed_out = true;
      _clean_stop_acknowledged = false;
    } else if (e == Event::StopAcknowledged && !_stop_timed_out) {
      _clean_stop_acknowledged = true;
    }

    if (r.effects.start_clients) _ops.startClients();
    if (r.effects.deliver_stop) _ops.deliverStop();
    if (r.effects.release_resources) _ops.releaseResources();
    if (r.effects.ota_release) _ops.onStopComplete(e == Event::StopAcknowledged);
    return true;
  }

  Ops& _ops;
  uint32_t _stop_timeout_ms;
  State _state = State::Stopped;
  uint32_t _stop_request_ms = 0;
  bool _stop_timed_out = false;
  bool _clean_stop_acknowledged = false;
};

}  // namespace MQTTLifecycle
