#pragma once

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <Arduino.h>
#include <helpers/IdentityStore.h>
#include <helpers/TxtDataHelpers.h>

#ifndef MQTT_IATA
  #define MQTT_IATA "XXX"
#endif

#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif

#ifndef WIFI_PWD
  #define WIFI_PWD ""
#endif

#ifndef MQTT_TOPIC_ROOT
  #define MQTT_TOPIC_ROOT "meshcore"
#endif

#ifndef MQTT_URI
  #define MQTT_URI ""
#endif

#ifndef MQTT_USERNAME
  #define MQTT_USERNAME ""
#endif

#ifndef MQTT_PASSWORD
  #define MQTT_PASSWORD ""
#endif

#ifndef MQTT_BROKER1_URI
  #define MQTT_BROKER1_URI MQTT_URI
#endif
#ifndef MQTT_BROKER1_USERNAME
  #define MQTT_BROKER1_USERNAME MQTT_USERNAME
#endif
#ifndef MQTT_BROKER1_PASSWORD
  #define MQTT_BROKER1_PASSWORD MQTT_PASSWORD
#endif
#ifndef MQTT_BROKER1_TOPIC_ROOT
  #define MQTT_BROKER1_TOPIC_ROOT MQTT_TOPIC_ROOT
#endif
#ifndef MQTT_BROKER1_IATA
  #define MQTT_BROKER1_IATA MQTT_IATA
#endif
#ifndef MQTT_BROKER1_RETAIN_STATUS
  #define MQTT_BROKER1_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER1_ENABLED
  #define MQTT_BROKER1_ENABLED 1
#endif

#ifndef MQTT_BROKER2_URI
  #define MQTT_BROKER2_URI ""
#endif
#ifndef MQTT_BROKER2_USERNAME
  #define MQTT_BROKER2_USERNAME ""
#endif
#ifndef MQTT_BROKER2_PASSWORD
  #define MQTT_BROKER2_PASSWORD ""
#endif
#ifndef MQTT_BROKER2_TOPIC_ROOT
  #define MQTT_BROKER2_TOPIC_ROOT ""
#endif
#ifndef MQTT_BROKER2_IATA
  #define MQTT_BROKER2_IATA ""
#endif
#ifndef MQTT_BROKER2_RETAIN_STATUS
  #define MQTT_BROKER2_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER2_ENABLED
  #define MQTT_BROKER2_ENABLED 0
#endif

#ifndef MQTT_BROKER3_URI
  #define MQTT_BROKER3_URI ""
#endif
#ifndef MQTT_BROKER3_USERNAME
  #define MQTT_BROKER3_USERNAME ""
#endif
#ifndef MQTT_BROKER3_PASSWORD
  #define MQTT_BROKER3_PASSWORD ""
#endif
#ifndef MQTT_BROKER3_TOPIC_ROOT
  #define MQTT_BROKER3_TOPIC_ROOT ""
#endif
#ifndef MQTT_BROKER3_IATA
  #define MQTT_BROKER3_IATA ""
#endif
#ifndef MQTT_BROKER3_RETAIN_STATUS
  #define MQTT_BROKER3_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER3_ENABLED
  #define MQTT_BROKER3_ENABLED 0
#endif

#ifndef MQTT_BROKER4_URI
  #define MQTT_BROKER4_URI ""
#endif
#ifndef MQTT_BROKER4_USERNAME
  #define MQTT_BROKER4_USERNAME ""
#endif
#ifndef MQTT_BROKER4_PASSWORD
  #define MQTT_BROKER4_PASSWORD ""
#endif
#ifndef MQTT_BROKER4_TOPIC_ROOT
  #define MQTT_BROKER4_TOPIC_ROOT ""
#endif
#ifndef MQTT_BROKER4_IATA
  #define MQTT_BROKER4_IATA ""
#endif
#ifndef MQTT_BROKER4_RETAIN_STATUS
  #define MQTT_BROKER4_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER4_ENABLED
  #define MQTT_BROKER4_ENABLED 0
#endif

#ifndef MQTT_BROKER5_URI
  #define MQTT_BROKER5_URI ""
#endif
#ifndef MQTT_BROKER5_USERNAME
  #define MQTT_BROKER5_USERNAME ""
#endif
#ifndef MQTT_BROKER5_PASSWORD
  #define MQTT_BROKER5_PASSWORD ""
#endif
#ifndef MQTT_BROKER5_TOPIC_ROOT
  #define MQTT_BROKER5_TOPIC_ROOT ""
#endif
#ifndef MQTT_BROKER5_IATA
  #define MQTT_BROKER5_IATA ""
#endif
#ifndef MQTT_BROKER5_RETAIN_STATUS
  #define MQTT_BROKER5_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER5_ENABLED
  #define MQTT_BROKER5_ENABLED 0
#endif

#ifndef MQTT_BROKER6_URI
  #define MQTT_BROKER6_URI ""
#endif
#ifndef MQTT_BROKER6_USERNAME
  #define MQTT_BROKER6_USERNAME ""
#endif
#ifndef MQTT_BROKER6_PASSWORD
  #define MQTT_BROKER6_PASSWORD ""
#endif
#ifndef MQTT_BROKER6_TOPIC_ROOT
  #define MQTT_BROKER6_TOPIC_ROOT ""
#endif
#ifndef MQTT_BROKER6_IATA
  #define MQTT_BROKER6_IATA ""
#endif
#ifndef MQTT_BROKER6_RETAIN_STATUS
  #define MQTT_BROKER6_RETAIN_STATUS MQTT_RETAIN_STATUS
#endif
#ifndef MQTT_BROKER6_ENABLED
  #define MQTT_BROKER6_ENABLED 0
#endif

#ifndef MQTT_STATUS_INTERVAL_SECS
  #define MQTT_STATUS_INTERVAL_SECS 60
#endif

#ifndef MQTT_CLIENT_VERSION
  #define MQTT_CLIENT_VERSION "custom-mqtt-observer/1.0.0"
#endif

#ifndef MQTT_MODEL
  #define MQTT_MODEL "Heltec V3"
#endif

#ifndef MQTT_NTP_SERVER
  #define MQTT_NTP_SERVER "pool.ntp.org"
#endif

#ifndef MQTT_RETAIN_STATUS
  #define MQTT_RETAIN_STATUS 1
#endif

static constexpr int MQTT_MAX_BROKERS = 6;

struct MqttSharedConfig {
  char wifi_ssid[64];
  char wifi_pwd[64];
  char model[64];
  char client_version[64];
};

struct MqttBrokerConfig {
  char uri[128];
  char username[64];
  char password[64];
  char topic_root[256];
  char iata[16];
  uint8_t retain_status;
  uint8_t enabled;
  uint8_t reserved[2];
};

// Legacy struct kept for v1 migration
struct MqttRuntimeConfig {
  char wifi_ssid[64];
  char wifi_pwd[64];
  char topic_root[256];
  char uri[128];
  char username[64];
  char password[64];
  char iata[16];
  char model[64];
  char client_version[64];
  uint8_t retain_status;
};

class MqttSettingsStore {
public:
  MqttSettingsStore();

  void begin(FILESYSTEM *fs);
  bool load();
  bool save();
  void resetToDefaults();

  const MqttSharedConfig &shared() const { return _shared; }
  const MqttBrokerConfig &broker(int idx) const { return _brokers[idx]; }
  int brokerCount() const;

private:
  // v1 persisted format (for migration)
  struct PersistedMqttConfigV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    MqttRuntimeConfig config;
  };

  // v2 persisted format
  struct PersistedMqttConfigV2 {
    uint32_t magic;
    uint16_t version;
    uint8_t broker_count;
    uint8_t reserved;
    MqttSharedConfig shared;
    MqttBrokerConfig brokers[MQTT_MAX_BROKERS];
  };

  FILESYSTEM *_fs;
  MqttSharedConfig _shared;
  MqttBrokerConfig _brokers[MQTT_MAX_BROKERS];

  static constexpr uint32_t CONFIG_MAGIC = 0x4D515454; // MQTT
  static constexpr uint16_t CONFIG_VERSION = 2;
  static constexpr const char *CONFIG_PATH = "/mqtt.cfg";

  bool loadV1(const uint8_t *data, size_t len);

  static void sanitizeShared(MqttSharedConfig &cfg);
  static void sanitizeBroker(MqttBrokerConfig &cfg);

  // Key parsing: returns broker index (0-based) and sets key_out to the
  // remaining key after stripping any "N." prefix. Returns -1 for shared keys.
  static int parseKey(const char *key, const char **key_out);

public:
  bool getValue(const char *key, char *dest, size_t dest_size, bool mask_secret = false) const;
  bool setValue(const char *key, const char *value);
};

#endif
