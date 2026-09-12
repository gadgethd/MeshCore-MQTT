// Native unit tests for MQTT clock acceptance, fallback, and refresh timing.

#include <gtest/gtest.h>

#include "helpers/MqttClockPolicy.h"

using mqtt_clock::Acceptance;
using mqtt_clock::ReplyState;
using mqtt_clock::Source;
using mqtt_clock::ValidationMode;
using mqtt_clock::ageMs;
using mqtt_clock::attemptDue;
using mqtt_clock::acceptExistingClock;
using mqtt_clock::evaluateNtpAttempt;
using mqtt_clock::plausibleEpoch;

namespace {
constexpr uint32_t kAttempt = 7;
constexpr uint32_t kFreshEpoch = 1893456000UL;  // 2030-01-01
constexpr uint32_t kOlderEpoch = 1859328000UL;  // 2028-12-01
}

TEST(MqttClockPolicy, RejectsKnownUnsetClockAndAcceptsPlausibleClock) {
  EXPECT_FALSE(plausibleEpoch(1715770351UL));  // shared 2024 cold-boot value
  EXPECT_TRUE(plausibleEpoch(kFreshEpoch));
  EXPECT_FALSE(plausibleEpoch(UINT32_MAX));
}

TEST(MqttClockPolicy, ValidExistingClockIsAnExplicitFallback) {
  Acceptance result = acceptExistingClock(kFreshEpoch);
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.fallback);
  EXPECT_FALSE(result.validated);
  EXPECT_EQ(result.source, Source::ExistingClockFallback);
  EXPECT_EQ(result.epoch, kFreshEpoch);

  EXPECT_FALSE(acceptExistingClock(1715770351UL).accepted);
}

TEST(MqttClockPolicy, CompletionRequiresCurrentPendingAttempt) {
  Acceptance stale = evaluateNtpAttempt(
      false, kAttempt, kAttempt, ReplyState::Completed, kFreshEpoch, true,
      ValidationMode::ValidatedExchange);
  EXPECT_FALSE(stale.accepted);

  Acceptance wrong_attempt = evaluateNtpAttempt(
      true, kAttempt, kAttempt + 1, ReplyState::Completed, kFreshEpoch, true,
      ValidationMode::ValidatedExchange);
  EXPECT_FALSE(wrong_attempt.accepted);
}

TEST(MqttClockPolicy, ValidatedModeRejectsMalformedOrMismatchedReplies) {
  for (ReplyState state : {ReplyState::Malformed, ReplyState::Mismatched}) {
    Acceptance result = evaluateNtpAttempt(
        true, kAttempt, kAttempt, state, kFreshEpoch, false,
        ValidationMode::ValidatedExchange);
    EXPECT_FALSE(result.accepted);
  }

  Acceptance invalid_payload = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Completed, kFreshEpoch, false,
      ValidationMode::ValidatedExchange);
  EXPECT_FALSE(invalid_payload.accepted);
}

TEST(MqttClockPolicy, ValidatedReplyIsMarkedValidated) {
  Acceptance result = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Completed, kFreshEpoch, true,
      ValidationMode::ValidatedExchange);
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.validated);
  EXPECT_FALSE(result.fallback);
  EXPECT_EQ(result.source, Source::NtpValidated);
}

TEST(MqttClockPolicy, ExplicitNoValidationIsVisibleAndNotValidated) {
  Acceptance result = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Completed, kFreshEpoch, false,
      ValidationMode::ExplicitNoValidation);
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.validated);
  EXPECT_EQ(result.source, Source::NtpCompletedNoValidation);
}

TEST(MqttClockPolicy, FailedNtpCanFallBackOnlyToValidExistingClock) {
  Acceptance valid = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Reset, 0, false,
      ValidationMode::ValidatedExchange, kFreshEpoch, true);
  EXPECT_TRUE(valid.accepted);
  EXPECT_TRUE(valid.fallback);

  Acceptance invalid = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Reset, 0, false,
      ValidationMode::ValidatedExchange, 1715770351UL, true);
  EXPECT_FALSE(invalid.accepted);
}

TEST(MqttClockPolicy, ClockStepResetsAcceptedAgeWithoutRequiringMonotonicTime) {
  Acceptance first = evaluateNtpAttempt(
      true, kAttempt, kAttempt, ReplyState::Completed, kFreshEpoch, true,
      ValidationMode::ValidatedExchange);
  Acceptance stepped = evaluateNtpAttempt(
      true, kAttempt + 1, kAttempt + 1, ReplyState::Completed, kOlderEpoch, true,
      ValidationMode::ValidatedExchange);
  EXPECT_TRUE(first.accepted);
  EXPECT_TRUE(stepped.accepted);
  EXPECT_EQ(stepped.epoch, kOlderEpoch);
  EXPECT_EQ(ageMs(true, 1000, 1250), 250u);
}

TEST(MqttClockPolicy, ValidatedRefreshIsPeriodicWhileFallbackRetries) {
  EXPECT_FALSE(attemptDue(true, false, Source::NtpValidated, 3599999, 0, 0));
  EXPECT_TRUE(attemptDue(true, false, Source::NtpValidated, 3600000, 0, 0));
  EXPECT_FALSE(attemptDue(true, false, Source::NtpCompletedNoValidation, 3599999, 0, 0));
  EXPECT_TRUE(attemptDue(true, false, Source::NtpCompletedNoValidation, 3600000, 0, 0));
  EXPECT_TRUE(attemptDue(true, false, Source::NtpValidated, 60000, 0, 1));
  EXPECT_FALSE(attemptDue(true, false, Source::ExistingClockFallback, 29999, 0, 0));
  EXPECT_TRUE(attemptDue(true, false, Source::ExistingClockFallback, 30000, 0, 0));
  EXPECT_FALSE(attemptDue(true, true, Source::ExistingClockFallback, 30000, 0, 0));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
