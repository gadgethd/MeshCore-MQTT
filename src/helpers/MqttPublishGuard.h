#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Pure publish-path decisions shared by the firmware and native tests.  The
// reporter still uses esp_mqtt_client_publish() for QoS 0 so it does not inherit
// esp-mqtt's approximately one-message/second queued-send behavior.  These
// limits instead bound each synchronous call and force a return to the mesh
// loop after a small, global (not per-broker) drain budget.
namespace mqtt_publish {

constexpr uint32_t kDrainElapsedBudgetMs = 250;
constexpr uint32_t kNetworkTimeoutMs = 2500;
constexpr uint8_t kDefaultMaxPublishesPerPass = 2;

constexpr size_t kConnectBufferBytes = 4096;
constexpr size_t kBuildBufferBytes = 4096;
constexpr size_t kMaxPublishPayloadBytes = kBuildBufferBytes - 1;
constexpr size_t kMaxLwtPayloadBytes = 3400;
constexpr size_t kMaxStatusTopicBytes = 383;
constexpr size_t kMaxClientIdBytes = 47;
constexpr size_t kMaxUsernameBytes = 63;
constexpr size_t kMaxPasswordBytes = 63;
constexpr size_t kJsonWorstCaseBytesPerInputByte = 6;

// MQTT 3.1.1 CONNECT: at this size the fixed header uses at most three bytes,
// and the protocol-name/version/flags/keepalive variable header uses ten.
constexpr size_t kConnectFixedWireBytes = 13;

constexpr size_t mqttWireStringBytes(size_t value_bytes) {
  return 2 + value_bytes;
}

constexpr size_t connectPacketBytes(size_t client_id_bytes,
                                    size_t username_bytes,
                                    size_t password_bytes,
                                    size_t lwt_topic_bytes,
                                    size_t lwt_payload_bytes) {
  return kConnectFixedWireBytes +
         mqttWireStringBytes(client_id_bytes) +
         mqttWireStringBytes(lwt_topic_bytes) +
         mqttWireStringBytes(lwt_payload_bytes) +
         mqttWireStringBytes(username_bytes) +
         mqttWireStringBytes(password_bytes);
}

constexpr size_t kMaxConfiguredConnectPacketBytes = connectPacketBytes(
    kMaxClientIdBytes,
    kMaxUsernameBytes,
    kMaxPasswordBytes,
    kMaxStatusTopicBytes,
    kMaxLwtPayloadBytes);

static_assert(kNetworkTimeoutMs <= 2500,
              "a stalled publish must return within the hard network bound");
static_assert(kDrainElapsedBudgetMs <= kNetworkTimeoutMs,
              "the normal drain budget must not exceed the hard call bound");
static_assert(kMaxLwtPayloadBytes <= kMaxPublishPayloadBytes,
              "the LWT must fit the reporter build buffer");
static_assert(kMaxConfiguredConnectPacketBytes <= kConnectBufferBytes,
              "the maximum LWT CONNECT packet must fit the 4-KiB buffers");

constexpr size_t jsonEscapedWorstCaseBytes(size_t input_bytes) {
  return input_bytes * kJsonWorstCaseBytesPerInputByte;
}

struct DrainState {
  uint32_t started_ms;
  uint8_t publish_calls;
};

constexpr DrainState startDrain(uint32_t now_ms) {
  return DrainState{now_ms, 0};
}

inline uint32_t elapsedMs(const DrainState &state, uint32_t now_ms) {
  return now_ms - state.started_ms;
}

inline bool mayPublish(const DrainState &state,
                       uint32_t now_ms,
                       uint8_t max_calls = kDefaultMaxPublishesPerPass,
                       uint32_t elapsed_budget_ms = kDrainElapsedBudgetMs) {
  return state.publish_calls < max_calls &&
         elapsedMs(state, now_ms) < elapsed_budget_ms;
}

inline void notePublishCall(DrainState &state) {
  if (state.publish_calls != UINT8_MAX) state.publish_calls++;
}

inline bool mustYieldToMesh(const DrainState &state,
                            uint32_t now_ms,
                            bool work_remaining,
                            uint8_t max_calls = kDefaultMaxPublishesPerPass,
                            uint32_t elapsed_budget_ms = kDrainElapsedBudgetMs) {
  return work_remaining && !mayPublish(state, now_ms, max_calls, elapsed_budget_ms);
}

inline uint8_t nextBroker(uint8_t current, uint8_t broker_count) {
  return broker_count == 0 ? 0 : (uint8_t)((current + 1) % broker_count);
}

constexpr size_t kQueueBrokerBytesInternal = 8 * 1024;
constexpr size_t kQueueTotalBytesInternal = 24 * 1024;
constexpr size_t kQueueBrokerBytesPsram = 16 * 1024;
constexpr size_t kQueueTotalBytesPsram = 96 * 1024;

inline size_t brokerByteCap(bool psram_available) {
  return psram_available ? kQueueBrokerBytesPsram : kQueueBrokerBytesInternal;
}

inline size_t totalByteCap(bool psram_available) {
  return psram_available ? kQueueTotalBytesPsram : kQueueTotalBytesInternal;
}

inline bool checkedAdd(size_t lhs, size_t rhs, size_t &result) {
  if (rhs > SIZE_MAX - lhs) return false;
  result = lhs + rhs;
  return true;
}

inline bool queueEntryBytes(size_t topic_bytes, size_t payload_bytes, size_t &result) {
  size_t strings = 0;
  if (!checkedAdd(topic_bytes, payload_bytes, strings)) return false;
  return checkedAdd(strings, 2, result);  // both owned copies include a NUL
}

enum class QueueAdmission : uint8_t {
  Accept = 0,
  EntryCap,
  BrokerByteCap,
  TotalByteCap,
  PayloadTooLarge,
  Overflow,
};

inline QueueAdmission admitQueueEntry(size_t queue_entries,
                                      size_t queue_entry_cap,
                                      size_t broker_bytes,
                                      size_t broker_byte_cap,
                                      size_t total_bytes,
                                      size_t total_byte_cap,
                                      size_t topic_bytes,
                                      size_t payload_bytes) {
  if (payload_bytes > kMaxPublishPayloadBytes) return QueueAdmission::PayloadTooLarge;
  if (queue_entries >= queue_entry_cap) return QueueAdmission::EntryCap;

  size_t entry_bytes = 0;
  if (!queueEntryBytes(topic_bytes, payload_bytes, entry_bytes)) {
    return QueueAdmission::Overflow;
  }
  if (entry_bytes > broker_byte_cap || broker_bytes > broker_byte_cap - entry_bytes) {
    return QueueAdmission::BrokerByteCap;
  }
  if (entry_bytes > total_byte_cap || total_bytes > total_byte_cap - entry_bytes) {
    return QueueAdmission::TotalByteCap;
  }
  return QueueAdmission::Accept;
}

// A no-allocation JSON/text builder.  Capacity includes the trailing NUL.
// fail_after_bytes is a native-test injection seam; firmware uses SIZE_MAX.
class CheckedBufferBuilder {
public:
  CheckedBufferBuilder(char *buffer, size_t capacity, size_t fail_after_bytes = SIZE_MAX)
      : _buffer(buffer),
        _capacity(capacity),
        _length(0),
        _fail_after(fail_after_bytes),
        _ok(buffer != nullptr && capacity > 0) {
    if (_ok) _buffer[0] = '\0';
  }

  bool ok() const { return _ok; }
  size_t length() const { return _length; }
  const char *c_str() const { return _buffer != nullptr ? _buffer : ""; }

  bool append(const char *value) {
    return value != nullptr && append(value, strlen(value));
  }

  bool append(const char *value, size_t value_bytes) {
    if (!_ok || value == nullptr || value_bytes > SIZE_MAX - _length) return fail();
    const size_t next = _length + value_bytes;
    if (next > _fail_after || next >= _capacity) return fail();
    if (value_bytes > 0) memcpy(_buffer + _length, value, value_bytes);
    _length = next;
    _buffer[_length] = '\0';
    return true;
  }

  bool appendChar(char value) {
    return append(&value, 1);
  }

  bool appendUnsigned(uint64_t value) {
    char digits[24];
    int written = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    return written > 0 && (size_t)written < sizeof(digits) && append(digits, (size_t)written);
  }

  bool appendSigned(int64_t value) {
    char digits[24];
    int written = snprintf(digits, sizeof(digits), "%lld", (long long)value);
    return written > 0 && (size_t)written < sizeof(digits) && append(digits, (size_t)written);
  }

  bool appendFloat1(double value) {
    char digits[40];
    int written = snprintf(digits, sizeof(digits), "%.1f", value);
    return written > 0 && (size_t)written < sizeof(digits) && append(digits, (size_t)written);
  }

  bool appendHex(const uint8_t *data, size_t data_bytes) {
    static const char kHex[] = "0123456789ABCDEF";
    if (data == nullptr && data_bytes != 0) return fail();
    if (data_bytes > (SIZE_MAX / 2)) return fail();
    for (size_t i = 0; i < data_bytes && _ok; ++i) {
      char encoded[2] = {kHex[data[i] >> 4], kHex[data[i] & 0x0F]};
      append(encoded, sizeof(encoded));
    }
    return _ok;
  }

  bool appendJsonEscaped(const char *value) {
    if (value == nullptr) return fail();
    static const char kHex[] = "0123456789ABCDEF";
    while (*value != '\0' && _ok) {
      const uint8_t c = (uint8_t)*value++;
      switch (c) {
        case '"': append("\\\""); break;
        case '\\': append("\\\\"); break;
        case '\b': append("\\b"); break;
        case '\f': append("\\f"); break;
        case '\n': append("\\n"); break;
        case '\r': append("\\r"); break;
        case '\t': append("\\t"); break;
        default:
          if (c < 0x20) {
            char escaped[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0F]};
            append(escaped, sizeof(escaped));
          } else {
            appendChar((char)c);
          }
          break;
      }
    }
    return _ok;
  }

private:
  bool fail() {
    _ok = false;
    if (_buffer != nullptr && _capacity > 0) {
      const size_t terminator = _length < _capacity ? _length : _capacity - 1;
      _buffer[terminator] = '\0';
    }
    return false;
  }

  char *_buffer;
  size_t _capacity;
  size_t _length;
  size_t _fail_after;
  bool _ok;
};

}  // namespace mqtt_publish
