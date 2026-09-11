#pragma once

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <Arduino.h>
#include <helpers/IdentityStore.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/MqttPrefsCodec.h>

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

#ifndef MQTT_NEIGHBOR_INTERVAL_SECS
  #define MQTT_NEIGHBOR_INTERVAL_SECS 900
#endif

#ifndef MQTT_NEIGHBOR_MIN_INTERVAL_SECS
  #define MQTT_NEIGHBOR_MIN_INTERVAL_SECS 300
#endif

#ifndef MQTT_CLIENT_VERSION
  #define MQTT_CLIENT_VERSION "meshcore-mqtt/v1.17.0"
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

// Codec types live in helpers/MqttPrefsCodec.h (pure, host-testable; layouts
// pinned with static_asserts there). The historical names are kept so all
// consumers are unchanged.
using MqttSharedConfig = mqtt_prefs::SharedConfig;
using MqttBrokerConfig = mqtt_prefs::BrokerConfig;

class MqttSettingsStore {
public:
  MqttSettingsStore();

  void begin(FILESYSTEM *fs);
  bool load();
  bool save();
  void resetToDefaults();
  void clearWriteHold();

  uint32_t bootCount() const { return _boot_count; }
  uint32_t incrementBootCount();
  uint32_t configCrc32() const;
  static uint16_t configVersion();

  const MqttSharedConfig &shared() const { return _shared; }
  const MqttBrokerConfig &broker(int idx) const { return _brokers[idx]; }
  int brokerCount() const;
  bool brokerCredentialsAllowed(int idx) const;

private:
  // Persisted layouts come from the codec (byte-pinned there); aliases keep
  // the historical names used across the store implementation.
  using PersistedMqttConfigV1 = mqtt_prefs::LegacyFileV1;
  using PersistedMqttConfigV2 = mqtt_prefs::LegacyFileV2;
  using PersistedMqttConfigV3 = mqtt_prefs::LegacyFileV3;
  using PersistedMqttConfigV4 = mqtt_prefs::FileV4;

  FILESYSTEM *_fs;
  MqttSharedConfig _shared;
  MqttBrokerConfig _brokers[MQTT_MAX_BROKERS];
  uint32_t _boot_count;
  bool _prefs_write_hold;

  static constexpr uint32_t CONFIG_MAGIC = 0x4D515454; // MQTT
  static constexpr uint16_t CONFIG_VERSION = 4;
  static constexpr const char *CONFIG_PATH = "/mqtt.cfg";
  static constexpr const char *CONFIG_TEMP_PATH = "/mqtt.cfg.tmp";
  static constexpr const char *CONFIG_BACKUP_PATH = "/mqtt.cfg.bak";

  bool loadPath(const char *path);
  bool loadV1(const uint8_t *data, size_t len);
  bool loadV2(const uint8_t *data, size_t len);
  bool saveConfigFile();
  bool saveBootCount();

  static void sanitizeShared(MqttSharedConfig &cfg);
  static void sanitizeBroker(MqttBrokerConfig &cfg);
  void loadBootCount();

  // Key parsing: returns broker index (0-based) and sets key_out to the
  // remaining key after stripping any "N." prefix. Returns -1 for shared keys.
  static int parseKey(const char *key, const char **key_out);

public:
  bool getValue(const char *key, char *dest, size_t dest_size, bool mask_secret = true) const;
  bool setValue(const char *key, const char *value);
};

#endif
