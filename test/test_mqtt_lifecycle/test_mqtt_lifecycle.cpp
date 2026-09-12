// Native unit tests for the MQTT reporter lifecycle and OTA flash barrier.

#include <cstdint>

#include <gtest/gtest.h>

#include "helpers/MQTTLifecycle.h"
#include "helpers/MQTTOTABarrier.h"

using MQTTLifecycle::Coordinator;
using MQTTLifecycle::Event;
using MQTTLifecycle::Ops;
using MQTTLifecycle::Result;
using MQTTLifecycle::State;
using MQTTLifecycle::acceptsNewWork;
using MQTTLifecycle::apply;
using MQTTLifecycle::elapsedMs;
using MQTTLifecycle::eventName;
using MQTTLifecycle::isStopInProgress;
using MQTTLifecycle::isStopUnproven;
using MQTTLifecycle::mayRestart;
using MQTTLifecycle::mayTouchOwnedState;
using MQTTLifecycle::stateName;
using MQTTLifecycle::suggestedStopTimeoutMs;

namespace {

constexpr uint32_t kT0 = 123456;
static_assert(suggestedStopTimeoutMs(6) >= 5000u + 8000u * 6u,
              "six-slot stop window must satisfy the audited 5+8n floor");

// Recording Ops double with a settable clock.
struct FakeOps : Ops {
  uint32_t now = kT0;
  int start_calls = 0;
  int stop_calls = 0;
  int release_calls = 0;
  int ota_calls = 0;
  bool last_clean = false;
  bool connecting = false;
  bool publishing = false;
  bool stopped_while_connecting = false;
  bool stopped_while_publishing = false;

  uint32_t nowMs() override { return now; }
  void startClients() override {
    start_calls++;
    connecting = true;
  }
  void deliverStop() override {
    stop_calls++;
    stopped_while_connecting = connecting;
    stopped_while_publishing = publishing;
    connecting = false;
    publishing = false;
  }
  void releaseResources() override { release_calls++; }
  void onStopComplete(bool clean) override {
    ota_calls++;
    last_clean = clean;
  }
};
}  // namespace

// --- pure transition function -------------------------------------------------

TEST(MqttLifecycle, FreshStateIsStoppedAndStartIsTheOnlyEvent) {
  Result r = apply(State::Stopped, Event::StopRequested);
  EXPECT_FALSE(r.accepted);  // idempotent stop
  EXPECT_EQ(r.next, State::Stopped);

  r = apply(State::Stopped, Event::StartRequested);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Starting);
  EXPECT_TRUE(r.effects.start_clients);
  EXPECT_FALSE(r.effects.release_resources);
}

TEST(MqttLifecycle, StartLifecycle) {
  Result r = apply(State::Starting, Event::StartCompleted);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Running);

  r = apply(State::Starting, Event::StartFailed);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Stopped);
  EXPECT_TRUE(r.effects.release_resources);
}

TEST(MqttLifecycle, StopDuringStartupIsAccepted) {
  Result r = apply(State::Starting, Event::StopRequested);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::StopRequested);
  EXPECT_TRUE(r.effects.deliver_stop);
}

TEST(MqttLifecycle, StopRequestedWalksToStopped) {
  Result r = apply(State::StopRequested, Event::StopBegan);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Stopping);

  r = apply(State::Stopping, Event::StopAcknowledged);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Stopped);
  EXPECT_TRUE(r.effects.release_resources);
  EXPECT_TRUE(r.effects.ota_release);

  // A reporter that skips the optional StopBegan ack may be acknowledged
  // directly from StopRequested.
  r = apply(State::StopRequested, Event::StopAcknowledged);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Stopped);
  EXPECT_TRUE(r.effects.release_resources);
  EXPECT_TRUE(r.effects.ota_release);
}

TEST(MqttLifecycle, TimeoutRetainsUnacknowledgedOwnership) {
  Result r = apply(State::StopRequested, Event::StopTimedOut);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::StopUnproven);
  EXPECT_FALSE(r.effects.release_resources);
  EXPECT_TRUE(r.effects.ota_release);

  r = apply(State::Stopping, Event::StopTimedOut);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::StopUnproven);
  EXPECT_FALSE(r.effects.release_resources);

  r = apply(State::StopUnproven, Event::StopAcknowledged);
  EXPECT_TRUE(r.accepted);
  EXPECT_EQ(r.next, State::Stopped);
  EXPECT_TRUE(r.effects.release_resources);
  EXPECT_FALSE(r.effects.ota_release);
}

TEST(MqttLifecycle, DuplicateEventsAreNoOps) {
  EXPECT_FALSE(apply(State::Running, Event::StartRequested).accepted);
  EXPECT_FALSE(apply(State::Running, Event::StartCompleted).accepted);
  EXPECT_FALSE(apply(State::StopRequested, Event::StopRequested).accepted);
  EXPECT_FALSE(apply(State::Stopping, Event::StopRequested).accepted);
  EXPECT_FALSE(apply(State::Stopped, Event::StopAcknowledged).accepted);
}

TEST(MqttLifecycle, Guards) {
  EXPECT_TRUE(acceptsNewWork(State::Starting));
  EXPECT_TRUE(acceptsNewWork(State::Running));
  EXPECT_FALSE(acceptsNewWork(State::StopRequested));
  EXPECT_FALSE(acceptsNewWork(State::Stopping));
  EXPECT_FALSE(acceptsNewWork(State::Stopped));

  EXPECT_TRUE(mayTouchOwnedState(State::StopRequested));
  EXPECT_TRUE(mayTouchOwnedState(State::Stopping));
  EXPECT_TRUE(mayTouchOwnedState(State::StopUnproven));
  EXPECT_FALSE(mayTouchOwnedState(State::Stopped));

  EXPECT_TRUE(mayRestart(State::Stopped));
  EXPECT_FALSE(mayRestart(State::Running));

  EXPECT_TRUE(isStopInProgress(State::StopRequested));
  EXPECT_TRUE(isStopInProgress(State::Stopping));
  EXPECT_TRUE(isStopUnproven(State::StopUnproven));
  EXPECT_FALSE(isStopInProgress(State::StopUnproven));
  EXPECT_FALSE(isStopInProgress(State::Running));
}

TEST(MqttLifecycle, NamesAreStable) {
  EXPECT_STREQ(stateName(State::Stopped), "Stopped");
  EXPECT_STREQ(stateName(State::Stopping), "Stopping");
  EXPECT_STREQ(stateName(State::StopUnproven), "StopUnproven");
  EXPECT_STREQ(eventName(Event::StopTimedOut), "StopTimedOut");
  EXPECT_STREQ(eventName(Event::StartRequested), "StartRequested");
}

TEST(MqttLifecycle, ElapsedHandlesRollover) {
  EXPECT_EQ(elapsedMs(1000, 400), 600u);
  // Wrap: then near UINT32_MAX, now just past it.
  uint32_t then = UINT32_MAX - 400u;
  uint32_t now = then + 1000u;  // wraps
  EXPECT_EQ(elapsedMs(now, then), 1000u);
}

TEST(MqttLifecycle, SuggestedTimeoutScalesWithSlots) {
  EXPECT_EQ(suggestedStopTimeoutMs(0), 13000u);  // clamped to one owned slot
  EXPECT_EQ(suggestedStopTimeoutMs(1), 13000u);
  EXPECT_EQ(suggestedStopTimeoutMs(2), 21000u);
  EXPECT_EQ(suggestedStopTimeoutMs(6), 53000u);
}

// --- Coordinator against the recording double ---------------------------------

TEST(MqttLifecycleCoordinator, HappyPathRecordsEffectsOnce) {
  FakeOps ops;
  Coordinator c(ops, suggestedStopTimeoutMs(1));

  EXPECT_EQ(c.state(), State::Stopped);
  EXPECT_TRUE(c.requestStart());
  EXPECT_EQ(c.state(), State::Starting);
  EXPECT_EQ(ops.start_calls, 1);

  EXPECT_FALSE(c.requestStart());  // idempotent
  EXPECT_EQ(ops.start_calls, 1);

  EXPECT_TRUE(c.onStarted());
  EXPECT_EQ(c.state(), State::Running);
  EXPECT_TRUE(c.acceptsNewWork());

  EXPECT_TRUE(c.requestStop());
  EXPECT_EQ(c.state(), State::StopRequested);
  EXPECT_EQ(ops.stop_calls, 1);

  EXPECT_TRUE(c.onStopBegan());
  EXPECT_EQ(c.state(), State::Stopping);

  EXPECT_TRUE(c.onStopped());
  EXPECT_EQ(c.state(), State::Stopped);
  EXPECT_EQ(ops.release_calls, 1);
  EXPECT_EQ(ops.ota_calls, 1);
  EXPECT_TRUE(ops.last_clean);
  EXPECT_TRUE(c.mayRestart());
  EXPECT_TRUE(c.mayBeginFlash());  // clean stop
}

TEST(MqttLifecycleCoordinator, FreshReporterIsFailClosedUntilVerifiedStop) {
  FakeOps ops;
  Coordinator c(ops, suggestedStopTimeoutMs(0));
  EXPECT_FALSE(c.mayBeginFlash());
}

TEST(MqttLifecycleCoordinator, TimeoutFiresOnlyAfterBound) {
  FakeOps ops;
  Coordinator c(ops, 6000);

  c.requestStart();
  c.onStarted();
  ops.now = kT0 + 500;
  c.requestStop();  // arms window at kT0+500

  ops.now = kT0 + 6499;  // 5999 ms elapsed
  c.tick();
  EXPECT_EQ(c.state(), State::StopRequested);
  EXPECT_EQ(ops.release_calls, 0);

  ops.now = kT0 + 6500;  // 6000 ms elapsed
  c.tick();
  EXPECT_EQ(c.state(), State::StopUnproven);
  EXPECT_TRUE(c.stopTimedOut());
  EXPECT_EQ(ops.release_calls, 0);
  EXPECT_EQ(ops.ota_calls, 1);
  EXPECT_FALSE(ops.last_clean);  // dirty stop
  EXPECT_FALSE(c.mayRestart());  // owner retained; no second lifecycle
  EXPECT_TRUE(c.mayTouchOwnedState());
  EXPECT_FALSE(c.mayBeginFlash());  // flashing withheld
}

TEST(MqttLifecycleCoordinator, LateAckReleasesButOriginalFlashAttemptStaysDirty) {
  FakeOps ops;
  Coordinator c(ops, 6000);

  c.requestStart();
  c.onStarted();
  c.requestStop();
  ops.now = kT0 + 7000;
  c.tick();  // dirty stop
  EXPECT_FALSE(c.mayBeginFlash());
  EXPECT_EQ(c.state(), State::StopUnproven);
  EXPECT_EQ(ops.release_calls, 0);

  EXPECT_FALSE(c.requestStart());  // no second owner before proof
  EXPECT_TRUE(c.onStopped());      // teardown eventually acknowledged
  EXPECT_EQ(c.state(), State::Stopped);
  EXPECT_EQ(ops.release_calls, 1);
  EXPECT_TRUE(c.stopTimedOut());
  EXPECT_FALSE(c.mayBeginFlash()); // this OTA attempt already failed closed

  EXPECT_TRUE(c.requestStart());   // a new lifecycle may now clear the latch
  EXPECT_FALSE(c.stopTimedOut());
  c.onStarted();
  c.requestStop();
  c.onStopped();  // clean this time
  EXPECT_TRUE(c.mayBeginFlash());
}

TEST(MqttLifecycleCoordinator, TimeoutSurvivesWrapAround) {
  FakeOps ops;
  Coordinator c(ops, 6000);
  ops.now = UINT32_MAX - 500u;

  c.requestStart();
  c.onStarted();
  c.requestStop();  // arms at UINT32_MAX-500

  c.tick();  // not yet (elapsed 0)
  EXPECT_EQ(c.state(), State::StopRequested);

  ops.now = (UINT32_MAX - 500u) + 6000u;  // wraps past zero
  c.tick();
  EXPECT_EQ(c.state(), State::StopUnproven);
  EXPECT_TRUE(c.stopTimedOut());
}

TEST(MqttLifecycleCoordinator, TimeoutUpdatableBeforeStop) {
  FakeOps ops;
  Coordinator c(ops, 1000);
  EXPECT_EQ(c.stopTimeoutMs(), 1000u);
  c.setStopTimeoutMs(suggestedStopTimeoutMs(2));
  EXPECT_EQ(c.stopTimeoutMs(), 21000u);

  c.requestStart();
  c.onStarted();
  c.requestStop();
  ops.now = kT0 + 20999;
  c.tick();
  EXPECT_EQ(c.state(), State::StopRequested);  // still within 21 s window
  ops.now = kT0 + 21000;
  c.tick();
  EXPECT_EQ(c.state(), State::StopUnproven);
}

TEST(MqttLifecycleCoordinator, StopDuringConnectAcksWithoutStartClientsRepeat) {
  FakeOps ops;
  Coordinator c(ops, 6000);
  c.requestStart();
  EXPECT_EQ(ops.start_calls, 1);
  c.requestStop();  // stop before init completes
  EXPECT_EQ(c.state(), State::StopRequested);
  EXPECT_EQ(ops.stop_calls, 1);
  EXPECT_TRUE(ops.stopped_while_connecting);
  EXPECT_FALSE(c.acceptsNewWork());
  EXPECT_EQ(ops.release_calls, 0);
  c.onStopped();  // reporter finished rollback
  EXPECT_EQ(c.state(), State::Stopped);
  EXPECT_TRUE(c.mayBeginFlash());
}

TEST(MqttLifecycleCoordinator, StopDuringPublishCeasesWorkBeforeRelease) {
  FakeOps ops;
  Coordinator c(ops, suggestedStopTimeoutMs(1));
  c.requestStart();
  ops.connecting = false;
  c.onStarted();
  ops.publishing = true;

  ASSERT_TRUE(c.requestStop());
  EXPECT_TRUE(ops.stopped_while_publishing);
  EXPECT_FALSE(c.acceptsNewWork());
  EXPECT_EQ(ops.release_calls, 0);
  c.onStopBegan();
  c.onStopped();
  EXPECT_EQ(ops.release_calls, 1);
  EXPECT_TRUE(c.mayBeginFlash());
}

TEST(MqttLifecycleCoordinator, RepeatedBeginEndCyclesAreIdempotent) {
  FakeOps ops;
  Coordinator c(ops, suggestedStopTimeoutMs(1));
  for (int cycle = 0; cycle < 3; cycle++) {
    ASSERT_TRUE(c.requestStart());
    EXPECT_FALSE(c.requestStart());
    ops.connecting = false;
    ASSERT_TRUE(c.onStarted());
    ASSERT_TRUE(c.requestStop());
    EXPECT_FALSE(c.requestStop());
    ASSERT_TRUE(c.onStopped());
  }
  EXPECT_EQ(ops.start_calls, 3);
  EXPECT_EQ(ops.stop_calls, 3);
  EXPECT_EQ(ops.release_calls, 3);
  EXPECT_EQ(ops.ota_calls, 3);
}

TEST(MqttOtaBarrier, NoFlashBeforeVerifiedStop) {
  MQTTOTA::Barrier barrier;
  ASSERT_TRUE(barrier.requestStop());
  EXPECT_FALSE(barrier.allowFlashIO(false));
  EXPECT_FALSE(barrier.flashStarted());

  barrier.reset();
  ASSERT_TRUE(barrier.requestStop());
  ASSERT_TRUE(barrier.onStopComplete(true));
  EXPECT_TRUE(barrier.allowFlashIO(true));
  EXPECT_TRUE(barrier.flashStarted());
}

TEST(MqttOtaBarrier, NoAckAndAbortedUploadNeverFlash) {
  MQTTOTA::Barrier no_ack;
  ASSERT_TRUE(no_ack.requestStop());
  ASSERT_TRUE(no_ack.onStopComplete(false));
  EXPECT_EQ(no_ack.state(), MQTTOTA::State::Aborted);
  EXPECT_FALSE(no_ack.allowFlashIO(false));

  MQTTOTA::Barrier aborted_upload;
  ASSERT_TRUE(aborted_upload.requestStop());
  ASSERT_TRUE(aborted_upload.onStopComplete(true));
  aborted_upload.abort();
  EXPECT_FALSE(aborted_upload.allowFlashIO(true));
  EXPECT_FALSE(aborted_upload.flashStarted());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
