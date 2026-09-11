// Native unit tests for the MQTT reconnect policy state machine.

#include <cstdint>

#include <gtest/gtest.h>

#include "helpers/MqttReconnectPolicy.h"

using mqtt_reconnect::State;
using mqtt_reconnect::attemptDue;
using mqtt_reconnect::kBreakerProbeMs;
using mqtt_reconnect::kStableGateMs;
using mqtt_reconnect::nextWaitMs;
using mqtt_reconnect::onConnected;
using mqtt_reconnect::onFailure;
using mqtt_reconnect::onTick;
using mqtt_reconnect::reset;
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
  onFailure(s, kT0);
  EXPECT_EQ(nextWaitMs(s, kT0), 10000u);
  onFailure(s, kT0 + 10000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 10000), 30000u);
  onFailure(s, kT0 + 40000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 40000), 60000u);
  onFailure(s, kT0 + 100000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 100000), 120000u);
  onFailure(s, kT0 + 220000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 220000), 300000u);
  onFailure(s, kT0 + 520000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 520000), 300000u);
}

TEST(MqttReconnectPolicy, AttemptDueOnlyAfterDelay) {
  State s{};
  onFailure(s, kT0);
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
  for (int i = 0; i < 4; i++) onFailure(s, kT0 + i);
  onFailure(s, kT0 + 10);
  onFailure(s, kT0 + 11);
  EXPECT_FALSE(s.breaker);
  onFailure(s, kT0 + 12);
  EXPECT_TRUE(s.breaker);
  EXPECT_EQ(nextWaitMs(s, kT0 + 12), kBreakerProbeMs);
  EXPECT_FALSE(attemptDue(s, kT0 + 12 + kBreakerProbeMs - 1));
  EXPECT_TRUE(attemptDue(s, kT0 + 12 + kBreakerProbeMs));
}

TEST(MqttReconnectPolicy, SuccessClearsBreakerButKeepsRung) {
  State s{};
  for (int i = 0; i < 7; i++) onFailure(s, kT0 + i);
  ASSERT_TRUE(s.breaker);
  onConnected(s, kT0 + 100);
  EXPECT_FALSE(s.breaker);
  EXPECT_EQ(s.rung, 4u);
  onFailure(s, kT0 + 30000);
  EXPECT_EQ(nextWaitMs(s, kT0 + 30000), 300000u);
}

TEST(MqttReconnectPolicy, StableGateResetsLadder) {
  State s{};
  onFailure(s, 0);
  onFailure(s, 10000);
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
  onFailure(s, 60000);  // dropped before the gate
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
  onFailure(s, 1000);
  EXPECT_FALSE(s.session_live);
  EXPECT_FALSE(s.attempt_in_flight);
}

TEST(MqttReconnectPolicy, ResetClearsEverything) {
  State s{};
  for (int i = 0; i < 7; i++) onFailure(s, i);
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
  onFailure(s, start);  // schedules at start + 10000, i.e. after wrap
  EXPECT_FALSE(attemptDue(s, UINT32_MAX - 4000u));
  uint32_t due = start + 10000u;
  EXPECT_TRUE(attemptDue(s, due));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
