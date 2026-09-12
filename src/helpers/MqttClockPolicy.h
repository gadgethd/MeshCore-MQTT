#pragma once

#include <stdint.h>

// Pure clock-acceptance decisions for the MQTT TLS gate. The firmware may use
// a framework-reported SNTP completion when the installed lwIP build does not
// enable response validation, but that mode remains visible as a distinct
// source and is never reported as a validated exchange.
namespace mqtt_clock {

constexpr uint32_t kMinimumTrustworthyEpoch = 1735689600UL;  // 2025-01-01
constexpr uint32_t kMaximumTrustworthyEpoch = 4102444800UL;  // 2100-01-01
constexpr uint32_t kNtpRetryIntervalMs = 30000UL;
constexpr uint32_t kNtpRetryMaxMs = 300000UL;
constexpr uint32_t kNtpRefreshIntervalMs = 60UL * 60UL * 1000UL;

enum class Source : uint8_t {
  None = 0,
  NtpValidated,
  NtpCompletedNoValidation,
  ExistingClockFallback,
};

enum class ValidationMode : uint8_t {
  ValidatedExchange = 0,
  ExplicitNoValidation,
};

enum class ReplyState : uint8_t {
  Reset = 0,
  InProgress,
  Completed,
  Malformed,
  Mismatched,
};

struct Acceptance {
  bool accepted;
  bool validated;
  bool fallback;
  Source source;
  uint32_t epoch;
};

inline Acceptance rejected() {
  Acceptance result = {};
  result.source = Source::None;
  return result;
}

inline bool plausibleEpoch(uint32_t epoch,
                           uint32_t minimum = kMinimumTrustworthyEpoch,
                           uint32_t maximum = kMaximumTrustworthyEpoch) {
  return epoch >= minimum && epoch <= maximum;
}

inline Acceptance acceptExistingClock(uint32_t epoch) {
  Acceptance result = rejected();
  if (!plausibleEpoch(epoch)) return result;
  result.accepted = true;
  result.fallback = true;
  result.source = Source::ExistingClockFallback;
  result.epoch = epoch;
  return result;
}

inline Acceptance evaluateNtpAttempt(
    bool request_pending,
    uint32_t attempt_id,
    uint32_t active_attempt_id,
    ReplyState reply,
    uint32_t ntp_epoch,
    bool response_valid,
    ValidationMode validation_mode,
    uint32_t existing_clock_epoch = 0,
    bool allow_existing_fallback = false) {
  Acceptance result = rejected();

  // A completion is only attributable to the attempt that is currently
  // pending. This rejects a status bit left over from a previous request.
  if (!request_pending || attempt_id == 0 || attempt_id != active_attempt_id) {
    return result;
  }

  if (reply == ReplyState::Completed && plausibleEpoch(ntp_epoch) &&
      (validation_mode == ValidationMode::ExplicitNoValidation || response_valid)) {
    result.accepted = true;
    result.validated = validation_mode == ValidationMode::ValidatedExchange;
    result.source = result.validated ? Source::NtpValidated
                                     : Source::NtpCompletedNoValidation;
    result.epoch = ntp_epoch;
    return result;
  }

  if (allow_existing_fallback) return acceptExistingClock(existing_clock_epoch);
  return result;
}

inline uint32_t retryDelayMs(uint8_t failures) {
  uint8_t shift = failures > 3 ? 3 : failures;
  uint32_t delay = kNtpRetryIntervalMs << shift;
  return delay > kNtpRetryMaxMs ? kNtpRetryMaxMs : delay;
}

inline bool isNtpSource(Source source) {
  return source == Source::NtpValidated ||
         source == Source::NtpCompletedNoValidation;
}

inline bool attemptDue(bool attempted, bool pending, Source source,
                       uint32_t now_ms, uint32_t last_attempt_ms,
                       uint8_t failures) {
  if (pending) return false;
  if (!attempted) return true;
  // A previously accepted NTP source is refreshed hourly when healthy. After
  // a failed refresh, retain the accepted source/age for diagnostics but retry
  // on the failure backoff rather than waiting another full hour.
  const uint32_t delay = isNtpSource(source) && failures == 0
                             ? kNtpRefreshIntervalMs
                             : retryDelayMs(failures);
  return (uint32_t)(now_ms - last_attempt_ms) >= delay;
}

inline uint32_t ageMs(bool accepted, uint32_t accepted_at_ms, uint32_t now_ms) {
  return accepted ? (uint32_t)(now_ms - accepted_at_ms) : 0;
}

inline bool isValidated(Source source) {
  return source == Source::NtpValidated;
}

inline bool isFallback(Source source) {
  return source == Source::ExistingClockFallback;
}

inline const char *sourceName(Source source) {
  switch (source) {
    case Source::NtpValidated: return "ntp_validated";
    case Source::NtpCompletedNoValidation: return "ntp_completed_no_validation";
    case Source::ExistingClockFallback: return "existing_clock_fallback";
    case Source::None: return "none";
    default: return "none";
  }
}

inline const char *validationModeName(ValidationMode mode) {
  return mode == ValidationMode::ValidatedExchange
             ? "validated_exchange"
             : "explicit_no_validation";
}

}  // namespace mqtt_clock
