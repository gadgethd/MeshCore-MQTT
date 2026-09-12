#pragma once

#include <stddef.h>
#include <stdint.h>

// Reporter-side containment for Packet::writeTo(), whose core contract assumes
// a valid Packet.  Keep this helper dependency-free so corrupt-source boundary
// cases can be tested natively without changing shared Packet code.
namespace mqtt_wire {

enum class Failure : uint8_t {
  None = 0,
  PayloadLength,
  PathEncoding,
  PathLength,
  RawLength,
};

struct Preflight {
  Failure failure;
  size_t path_bytes;
  size_t raw_bytes;

  bool ok() const { return failure == Failure::None; }
};

inline Preflight validate(size_t payload_len,
                          size_t encoded_path_len,
                          bool has_transport_codes,
                          size_t max_payload_len,
                          size_t max_path_bytes,
                          size_t raw_capacity) {
  if (payload_len > max_payload_len) {
    return Preflight{Failure::PayloadLength, 0, 0};
  }
  if (encoded_path_len > UINT8_MAX) {
    return Preflight{Failure::PathEncoding, 0, 0};
  }

  const uint8_t encoded = (uint8_t)encoded_path_len;
  const size_t hash_count = encoded & 63U;
  const size_t hash_size = (encoded >> 6U) + 1U;
  if (hash_size == 4U) {
    return Preflight{Failure::PathEncoding, 0, 0};
  }

  const size_t path_bytes = hash_count * hash_size;
  if (path_bytes > max_path_bytes) {
    return Preflight{Failure::PathLength, path_bytes, 0};
  }

  const size_t prefix_bytes = has_transport_codes ? 6U : 2U;
  if (path_bytes > SIZE_MAX - prefix_bytes ||
      payload_len > SIZE_MAX - prefix_bytes - path_bytes) {
    return Preflight{Failure::RawLength, path_bytes, 0};
  }
  const size_t raw_bytes = prefix_bytes + path_bytes + payload_len;
  if (raw_bytes > raw_capacity || raw_bytes > UINT8_MAX) {
    return Preflight{Failure::RawLength, path_bytes, raw_bytes};
  }
  return Preflight{Failure::None, path_bytes, raw_bytes};
}

}  // namespace mqtt_wire
