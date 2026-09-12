// Native unit tests for the MQTT reconnect policy state machine.

#include <cstdint>

#include <gtest/gtest.h>

#include "helpers/MqttReconnectPolicy.h"

using mqtt_reconnect::State;
using mqtt_reconnect::AttemptGuard;
using mqtt_reconnect::attemptDue;
using mqtt_reconnect::generationMatches;
using mqtt_reconnect::guardAllowsAttempt;
using mqtt_reconnect::kAttemptTimeoutMs;
using mqtt_reconnect::kBreakerProbeMs;
using mqtt_reconnect::kReconnectGuardMs;
using mqtt_reconnect::kStableGateMs;
using mqtt_reconnect::nextGeneration;
using mqtt_reconnect::nextWaitMs;
using mqtt_reconnect::noteAttempt;
using mqtt_reconnect::onAttemptStarted;
using mqtt_reconnect::onConnected;
using mqtt_reconnect::onFailure;
using mqtt_reconnect::onTerminalEvent;
using mqtt_reconnect::onTick;
using mqtt_reconnect::onWifiRecovered;
using mqtt_reconnect::reconcileAttemptTimeout;
using mqtt_reconnect::reset;
using mqtt_reconnect::resetGuard;
using mqtt_reconnect::seedInitial;

namespace {
constexpr uint32_t kT0 = 100000;
}

TEST(MqttReconnectPolicy, FreshStateIsDueImmediately) {
  State s{};
  EXPECT_TRUE(attemptDue(s, kT0));
  EXPECT_EQ(nextWaitMs(s, kT0), 0u);
}

TEST(MqttReconnectPolicy, SeedStaggersSlots) {
  for (uint8_t slot = 0; slot < 4; slot++) {
    State s{};
    seedInitial(s, kT0, slot);
    uint32_t due = kT0 + slot * 3000u;
    if (slot == 0) {
      // Slot 0 is due immediately (stagger 0).
      EXPECT_TRUE(attemptDue(s, kT0));
    } else {
      EXPECT_FALSE(attemptDue(s, kT0)) << "slot " << (int)slot;
      EXPECT_FALSE(attemptDue(s, due - 1)) << "slot " << (int)slot;
    }
    EXPECT_TRUE(attemptDue(s, due)) << "slot " << (int)slot;
  }
}

TEST(MqttReconnectPolicy, FailureWalksTheLadder) {
  State s{};
  onFailure(s, kT0, 0);
  EXPECT_EQ(nextWaitMs(s, kT0), 10000u);
  onFailure(s, kT0 + 10000, 0);
  EXPECT_EQ(nextWaitMs(s, kT0 + 10000), 30000u);
  onFailure(s, kT0 + 40000, 0);
  EXPECT_EQ(nextWaitMs(s, kT0 + 40000), 60000u);
  onFailure(s, kT0 + 100000, 0);
  EXPECT_EQ(nextWaitMs(s, kT0 + 100000), 120000u);
  onFailure(s, kT0 + 220000, 0);
  EXPECT_EQ(nextWaitMs(s, kT0 + 220000), 300000u);
  onFailure(s, kT0 + 520000, 0);
  EXPECT_EQ(nextWaitMs(s, kT0 + 520000), 300000u);
}

TEST(MqttReconnectPolicy, AttemptDueOnlyAfterDelay) {
  State s{};
  onFailure(s, kT0, 0);
  EXPECT_FALSE(attemptDue(s, kT0));
  EXPECT_FALSE(attemptDue(s, kT0 + 9999));
  EXPECT_TRUE(attemptDue(s, kT0 + 10000));
}

TEST(MqttReconnectPolicy, InFlightAttemptBlocksDue) {
  State s{};
  s.attempt_in_flight = true;
  EXPECT_FALSE(attemptDue(s, kT0 + 100000));
  s.attempt_in_flight = false;
  EXPECT_TRUE(attemptDue(s, kT0 + 100000));
}

TEST(MqttReconnectPolicy, BreakerLatchesAfterThreeFailuresAtCap) {
  State s{};
  // Four failures walk the rungs 0..4; then three more at the cap.
  for (int i = 0; i < 4; i++) onFailure(s, kT0 + i, 0);
  onFailure(s, kT0 + 10, 0);
  onFailure(s, kT0 + 11, 0);
  EXPECT_FALSE(s.breaker);
  onFailure(s, kT0 + 12, 0);
  EXPECT_TRUE(s.breaker);
  EXPECT_EQ(nextWaitMs(s, kT0 + 12), kBreakerProbeMs);
  EXPECT_FALSE(attemptDue(s, kT0 + 12 + kBreakerProbeMs - 1));
  EXPECT_TRUE(attemptDue(s, kT0 + 12 + kBreakerProbeMs));
}

TEST(MqttReconnectPolicy, SuccessClearsBreakerButKeepsRung) {
  State s{};
  for (int i = 0; i < 7; i++) onFailure(s, kT0 + i, 0);
  ASSERT_TRUE(s.breaker);
  onConnected(s, kT0 + 100);
  EXPECT_FALSE(s.breaker);
  EXPECT_EQ(s.rung, 4u);
  EXPECT_EQ(s.cap_failures, 3u);
  onFailure(s, kT0 + 30000, 0);
  EXPECT_TRUE(s.breaker);
  EXPECT_EQ(nextWaitMs(s, kT0 + 30000), kBreakerProbeMs);
}

TEST(MqttReconnectPolicy, StableGateResetsLadder) {
  State s{};
  onFailure(s, 0, 0);
  onFailure(s, 10000, 0);
  EXPECT_EQ(s.rung, 2u);
  onConnected(s, 50000);
  onTick(s, 50000 + 119999);
  EXPECT_EQ(s.rung, 2u);
  onTick(s, 50000 + kStableGateMs);
  EXPECT_EQ(s.rung, 0u);
  EXPECT_FALSE(s.breaker);
  EXPECT_TRUE(s.connected);
  EXPECT_EQ(s.cap_failures, 0u);
}

TEST(MqttReconnectPolicy, FlapKeepsAccumulatedBackoff) {
  State s{};
  onConnected(s, 0);
  onTick(s, 60000);
  EXPECT_EQ(s.rung, 0u);
  onFailure(s, 60000, 0);  // dropped before the gate
  EXPECT_EQ(s.rung, 1u);
  onConnected(s, 70000);
  onTick(s, 70000 + kStableGateMs);  // stable this time
  EXPECT_EQ(s.rung, 0u);
}

TEST(MqttReconnectPolicy, SessionFlagsTrackDrops) {
  State s{};
  EXPECT_FALSE(s.session_live);
  onConnected(s, 0);
  EXPECT_TRUE(s.session_live);
  EXPECT_FALSE(s.attempt_in_flight);
  onFailure(s, 1000, 0);
  EXPECT_FALSE(s.session_live);
  EXPECT_FALSE(s.attempt_in_flight);
}

TEST(MqttReconnectPolicy, ResetClearsEverything) {
  State s{};
  for (int i = 0; i < 7; i++) onFailure(s, i, 0);
  seedInitial(s, 0, 3);
  reset(s);
  EXPECT_EQ(s.rung, 0u);
  EXPECT_FALSE(s.breaker);
  EXPECT_FALSE(s.attempt_pending);
  EXPECT_FALSE(s.attempt_in_flight);
  EXPECT_TRUE(attemptDue(s, 12345));
}

TEST(MqttReconnectPolicy, TimingWrapsAround) {
  State s{};
  uint32_t start = UINT32_MAX - 5000u;
  onFailure(s, start, 0);  // schedules at start + 10000, i.e. after wrap
  EXPECT_FALSE(attemptDue(s, UINT32_MAX - 4000u));
  uint32_t due = start + 10000u;
  EXPECT_TRUE(attemptDue(s, due));
}

TEST(MqttReconnectPolicy, RetryDelayIncludesSlotStagger) {
  for (uint8_t slot = 0; slot < 4; slot++) {
    State s{};
    onAttemptStarted(s, kT0);
    ASSERT_TRUE(onTerminalEvent(s, kT0 + 100, slot));
    EXPECT_EQ(nextWaitMs(s, kT0 + 100), 10000u + (uint32_t)slot * 3000u);
  }
}

TEST(MqttReconnectPolicy, CrossSlotGuardAllowsOneAttemptPerPass) {
  State slots[2] = {};
  AttemptGuard guard{};
  bool attempted_this_pass = false;
  int attempts = 0;

  for (State &slot : slots) {
    if (attemptDue(slot, kT0) && guardAllowsAttempt(guard, kT0, attempted_this_pass)) {
      onAttemptStarted(slot, kT0);
      noteAttempt(guard, kT0);
      attempted_this_pass = true;
      attempts++;
    }
  }
  EXPECT_EQ(attempts, 1);

  attempted_this_pass = false;
  EXPECT_FALSE(guardAllowsAttempt(guard, kT0 + kReconnectGuardMs - 1,
                                  attempted_this_pass));
  EXPECT_TRUE(guardAllowsAttempt(guard, kT0 + kReconnectGuardMs,
                                 attempted_this_pass));
}

TEST(MqttReconnectPolicy, StableGatePreservesTopRungCountUntilProven) {
  State s{};
  s.rung = 4;
  s.cap_failures = 2;
  onAttemptStarted(s, kT0);
  ASSERT_TRUE(onConnected(s, kT0 + 100));
  EXPECT_EQ(s.cap_failures, 2u);
  onTick(s, kT0 + 100 + kStableGateMs - 1);
  EXPECT_EQ(s.cap_failures, 2u);
  EXPECT_EQ(s.rung, 4u);
  onTick(s, kT0 + 100 + kStableGateMs);
  EXPECT_EQ(s.cap_failures, 0u);
  EXPECT_EQ(s.rung, 0u);
}

TEST(MqttReconnectPolicy, FiveSecondFlapperReachesBreaker) {
  State s{};
  uint32_t now = kT0;
  int handshakes = 0;

  while (!s.breaker && handshakes < 10) {
    ASSERT_TRUE(attemptDue(s, now));
    onAttemptStarted(s, now);
    ASSERT_TRUE(onConnected(s, now));
    handshakes++;
    now += 5000u;
    ASSERT_TRUE(onTerminalEvent(s, now, 0));
    if (!s.breaker) now += nextWaitMs(s, now);
  }

  EXPECT_TRUE(s.breaker);
  EXPECT_EQ(handshakes, 7);
  EXPECT_EQ(s.cap_failures, 3u);
  EXPECT_EQ(nextWaitMs(s, now), kBreakerProbeMs);

  now += kBreakerProbeMs;
  ASSERT_TRUE(attemptDue(s, now));
  onAttemptStarted(s, now);
  ASSERT_TRUE(onConnected(s, now));
  EXPECT_EQ(s.cap_failures, 3u);
  ASSERT_TRUE(onTerminalEvent(s, now + 5000u, 0));
  EXPECT_TRUE(s.breaker);
  EXPECT_EQ(nextWaitMs(s, now + 5000u), kBreakerProbeMs);
}

TEST(MqttReconnectPolicy, DeliveredErrorMakesDroppedDisconnectHarmless) {
  State s{};
  onAttemptStarted(s, kT0);

  // ERROR is delivered; the paired DISCONNECTED callback is lost. The ERROR
  // transition alone schedules recovery, and a late duplicate is a no-op.
  ASSERT_TRUE(onTerminalEvent(s, kT0 + 100, 1));
  EXPECT_FALSE(onTerminalEvent(s, kT0 + 101, 1));
  EXPECT_FALSE(attemptDue(s, kT0 + 100 + 12999u));
  EXPECT_TRUE(attemptDue(s, kT0 + 100 + 13000u));
}

TEST(MqttReconnectPolicy, QueueUnavailableTerminalFallbackRecovers) {
  State s{};
  onAttemptStarted(s, kT0);

  // The reporter's out-of-band fallback invokes this transition when the
  // callback queue is null/full; no queued event is required for recovery.
  ASSERT_TRUE(onTerminalEvent(s, kT0 + 250, 0));
  EXPECT_TRUE(s.attempt_pending);
  EXPECT_FALSE(s.attempt_in_flight);
  EXPECT_TRUE(attemptDue(s, kT0 + 250 + 10000u));
}

TEST(MqttReconnectPolicy, DuplicateAndDisorderedTerminalEventsAreIdempotent) {
  State s{};
  onAttemptStarted(s, kT0);
  ASSERT_TRUE(onConnected(s, kT0 + 10));
  EXPECT_FALSE(onConnected(s, kT0 + 20));
  EXPECT_EQ(s.connected_since_ms, kT0 + 10);

  // DISCONNECTED arriving before ERROR still produces one failure step.
  ASSERT_TRUE(onTerminalEvent(s, kT0 + 100, 0));
  EXPECT_FALSE(onTerminalEvent(s, kT0 + 101, 0));
  EXPECT_EQ(s.rung, 1u);
  EXPECT_EQ(nextWaitMs(s, kT0 + 100), 10000u);
}

TEST(MqttReconnectPolicy, OrphanedInFlightAttemptTimesOutAndRetries) {
  State s{};
  onAttemptStarted(s, kT0);
  EXPECT_FALSE(reconcileAttemptTimeout(s, kT0 + kAttemptTimeoutMs - 1, 2));
  ASSERT_TRUE(reconcileAttemptTimeout(s, kT0 + kAttemptTimeoutMs, 2));
  EXPECT_FALSE(s.attempt_in_flight);
  EXPECT_TRUE(s.attempt_pending);
  EXPECT_EQ(nextWaitMs(s, kT0 + kAttemptTimeoutMs), 16000u);
  EXPECT_TRUE(attemptDue(s, kT0 + kAttemptTimeoutMs + 16000u));
}

TEST(MqttReconnectPolicy, StaleGenerationsAreRejectedAcrossPointerReuse) {
  uint32_t old_generation = nextGeneration(0);
  uint32_t active_generation = nextGeneration(old_generation);
  EXPECT_FALSE(generationMatches(active_generation, old_generation));
  EXPECT_TRUE(generationMatches(active_generation, active_generation));
  EXPECT_FALSE(generationMatches(0, 0));
  EXPECT_EQ(nextGeneration(UINT32_MAX), 1u);
}

TEST(MqttReconnectPolicy, WifiRecoveryReplacesObsoleteDeadline) {
  State s{};
  s.rung = 4;
  s.cap_failures = 2;
  s.breaker = true;
  s.attempt_pending = true;
  s.next_attempt_ms = kT0 + kBreakerProbeMs;

  onWifiRecovered(s, kT0 + 1000u, 2);
  EXPECT_EQ(s.rung, 1u);
  EXPECT_EQ(s.cap_failures, 2u);
  EXPECT_TRUE(s.breaker);
  EXPECT_FALSE(attemptDue(s, kT0 + 6999u));
  EXPECT_TRUE(attemptDue(s, kT0 + 7000u));
}

TEST(MqttReconnectPolicy, ResetClearsAttemptGuard) {
  AttemptGuard guard{};
  noteAttempt(guard, kT0);
  ASSERT_FALSE(guardAllowsAttempt(guard, kT0 + 1, false));
  resetGuard(guard);
  EXPECT_TRUE(guardAllowsAttempt(guard, kT0 + 1, false));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
