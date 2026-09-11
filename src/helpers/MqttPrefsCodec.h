#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure, dependency-free codec + migration logic for the MQTT settings file
// (/mqtt.cfg). Extracted so the on-disk layouts and every v1/v2/v3 -> v4
// migration can be exercised with host tests against frozen binary fixtures
// (never on-device-only). The store keeps all I/O and build-flag defaults; it
// composes the file piecewise (nothing multi-KB on the stack - that overflow
// rebooted nodes on every `set`) and maps legacy records through the helpers
// here.
//
// Layout discipline: field order and sizes are append-only within a version
// and pinned with static_asserts (verified byte-identical on g++ host and the
// xtensa ESP32-S3 toolchain). Any change requires a version bump.
namespace mqtt_prefs {

constexpr uint32_t kMagic = 0x4D515454UL;  // 'MQTT'
constexpr uint16_t kVersion = 4;
constexpr int kMaxBrokers = 6;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint8_t broker_count;
  uint8_t reserved;
};

struct SharedConfig {
  char wifi_ssid[64];
  char wifi_pwd[64];
  char model[64];
  char client_version[64];
};

struct BrokerConfig {
  char uri[128];
  char username[64];
  char password[64];
  char topic_root[256];
  char iata[16];
  uint8_t retain_status;
  uint8_t enabled;
  uint8_t reserved[2];
  uint32_t neighbor_interval_secs;
};

// Current on-disk image (header + shared + brokers). The on-device writer
// composes these pieces individually; this aggregate exists for tests and
// tooling (serializeV4 below).
struct FileV4 {
  Header header;
  SharedConfig shared;
  BrokerConfig brokers[kMaxBrokers];
};

// ---- legacy layouts (kept for migration; do not reorder) ----

// v1: one broker only; topic_root was 32 bytes.
struct LegacyRuntimeConfigV1 {
  char wifi_ssid[64];
  char wifi_pwd[64];
  char topic_root[32];
  char uri[128];
  char username[64];
  char password[64];
  char iata[16];
  char model[64];
  char client_version[64];
  uint8_t retain_status;
};

struct LegacyFileV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  LegacyRuntimeConfigV1 config;
};

// v2: full shared block + 6 brokers; topic_root still 32 bytes.
struct LegacyBrokerConfigV2 {
  char uri[128];
  char username[64];
  char password[64];
  char topic_root[32];
  char iata[16];
  uint8_t retain_status;
  uint8_t enabled;
  uint8_t reserved[2];
};

struct LegacyFileV2 {
  uint32_t magic;
  uint16_t version;
  uint8_t broker_count;
  uint8_t reserved;
  SharedConfig shared;
  LegacyBrokerConfigV2 brokers[kMaxBrokers];
};

// v3: topic_root widened to 256 bytes; no neighbor interval yet.
struct LegacyBrokerConfigV3 {
  char uri[128];
  char username[64];
  char password[64];
  char topic_root[256];
  char iata[16];
  uint8_t retain_status;
  uint8_t enabled;
  uint8_t reserved[2];
};

struct LegacyFileV3 {
  uint32_t magic;
  uint16_t version;
  uint8_t broker_count;
  uint8_t reserved;
  SharedConfig shared;
  LegacyBrokerConfigV3 brokers[kMaxBrokers];
};

// ---- layout pins (byte-identical on g++ host and xtensa ESP32-S3) ----
static_assert(sizeof(Header) == 8, "layout drift");
static_assert(sizeof(SharedConfig) == 256, "layout drift");
static_assert(sizeof(BrokerConfig) == 536, "layout drift");
static_assert(sizeof(LegacyRuntimeConfigV1) == 561, "layout drift");
static_assert(sizeof(LegacyFileV1) == 572, "layout drift");
static_assert(sizeof(LegacyBrokerConfigV2) == 308, "layout drift");
static_assert(sizeof(LegacyFileV2) == 2112, "layout drift");
static_assert(sizeof(LegacyBrokerConfigV3) == 532, "layout drift");
static_assert(sizeof(LegacyFileV3) == 3456, "layout drift");
static_assert(sizeof(FileV4) == 3480, "layout drift");

// What a stored file's header says. Unrecognized => the store preserves the
// file and holds writes (downgrade safety).
enum class Kind : uint8_t {
  TooSmall = 0,
  BadMagic,
  V1,
  V2,
  V3,
  V4,
  Unrecognized,
};

inline Kind classify(const uint8_t *data, size_t len) {
  if (len < sizeof(Header)) return Kind::TooSmall;
  Header hdr;
  const uint8_t *p = data;
  for (size_t i = 0; i < sizeof(hdr); i++) reinterpret_cast<uint8_t *>(&hdr)[i] = p[i];
  if (hdr.magic != kMagic) return Kind::BadMagic;
  switch (hdr.version) {
    case 1: return Kind::V1;
    case 2: return Kind::V2;
    case 3: return Kind::V3;
    case kVersion: return Kind::V4;
    default: return Kind::Unrecognized;
  }
}

// Bounded copy with StrHelper::strncpy semantics (up to buf_sz-1 chars, then a
// single terminator; no zero-fill) - kept identical so migrated strings are
// byte-for-byte what the fielded firmware produced.
inline void copyStr(char *dest, const char *src, size_t buf_sz) {
  while (buf_sz > 1 && *src) {
    *dest++ = *src++;
    buf_sz--;
  }
  *dest = 0;
}

// ---- v1 -> v4 mapping ----
inline void mapSharedFromV1(const LegacyFileV1 &src, SharedConfig &dst) {
  copyStr(dst.wifi_ssid, src.config.wifi_ssid, sizeof(dst.wifi_ssid));
  copyStr(dst.wifi_pwd, src.config.wifi_pwd, sizeof(dst.wifi_pwd));
  copyStr(dst.model, src.config.model, sizeof(dst.model));
  copyStr(dst.client_version, src.config.client_version, sizeof(dst.client_version));
}

inline void mapBrokerFromV1(const LegacyRuntimeConfigV1 &src, BrokerConfig &dst) {
  copyStr(dst.uri, src.uri, sizeof(dst.uri));
  copyStr(dst.username, src.username, sizeof(dst.username));
  copyStr(dst.password, src.password, sizeof(dst.password));
  copyStr(dst.topic_root, src.topic_root, sizeof(dst.topic_root));
  copyStr(dst.iata, src.iata, sizeof(dst.iata));
  dst.retain_status = src.retain_status ? 1 : 0;
  dst.enabled = 1;  // v1 had a single, implicitly-enabled broker
}

// ---- v2 -> v4 mapping ----
inline void mapBrokerFromV2(const LegacyBrokerConfigV2 &src, BrokerConfig &dst) {
  copyStr(dst.uri, src.uri, sizeof(dst.uri));
  copyStr(dst.username, src.username, sizeof(dst.username));
  copyStr(dst.password, src.password, sizeof(dst.password));
  copyStr(dst.topic_root, src.topic_root, sizeof(dst.topic_root));
  copyStr(dst.iata, src.iata, sizeof(dst.iata));
  dst.retain_status = src.retain_status;
  dst.enabled = src.enabled;
}

// ---- v3 -> v4 mapping ----
// neighbor_interval_secs did not exist in v3; the caller supplies the
// configured default (a build-flag concern, not a codec concern).
inline void mapBrokerFromV3(const LegacyBrokerConfigV3 &src, BrokerConfig &dst,
                            uint32_t neighbor_interval_default) {
  copyStr(dst.uri, src.uri, sizeof(dst.uri));
  copyStr(dst.username, src.username, sizeof(dst.username));
  copyStr(dst.password, src.password, sizeof(dst.password));
  copyStr(dst.topic_root, src.topic_root, sizeof(dst.topic_root));
  copyStr(dst.iata, src.iata, sizeof(dst.iata));
  dst.retain_status = src.retain_status;
  dst.enabled = src.enabled;
  dst.neighbor_interval_secs = neighbor_interval_default;
}

// ---- serialization (tests/tooling; on-device composes piecewise over File) ----
inline void serializeV4(uint8_t broker_count, const SharedConfig &shared,
                        const BrokerConfig *brokers, uint8_t out[sizeof(FileV4)]) {
  Header hdr;
  hdr.magic = kMagic;
  hdr.version = kVersion;
  hdr.broker_count = broker_count;
  hdr.reserved = 0;
  size_t off = 0;
  const uint8_t *pieces[3] = {reinterpret_cast<const uint8_t *>(&hdr),
                              reinterpret_cast<const uint8_t *>(&shared),
                              reinterpret_cast<const uint8_t *>(brokers)};
  const size_t sizes[3] = {sizeof(hdr), sizeof(shared), sizeof(BrokerConfig) * kMaxBrokers};
  for (int i = 0; i < 3; i++) {
    for (size_t b = 0; b < sizes[i]; b++) out[off + b] = pieces[i][b];
    off += sizes[i];
  }
}

// ---- CRC32 (zlib polynomial), same bytes the store displays ----
inline void crc32Update(uint32_t &crc, const uint8_t *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
}

inline uint32_t crc32Finish(uint32_t crc) { return ~crc; }

}  // namespace mqtt_prefs
