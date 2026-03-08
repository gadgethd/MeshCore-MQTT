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

struct MqttRuntimeConfig {
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

class MqttSettingsStore {
public:
  MqttSettingsStore();

  void begin(FILESYSTEM *fs);
  bool load();
  bool save();
  void resetToDefaults();

  const MqttRuntimeConfig &config() const { return _config; }

  bool getValue(const char *key, char *dest, size_t dest_size, bool mask_secret = false) const;
  bool setValue(const char *key, const char *value);

private:
  struct PersistedMqttConfig {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    MqttRuntimeConfig config;
  };

  FILESYSTEM *_fs;
  MqttRuntimeConfig _config;

  static constexpr uint32_t CONFIG_MAGIC = 0x4D515454; // MQTT
  static constexpr uint16_t CONFIG_VERSION = 1;
  static constexpr const char *CONFIG_PATH = "/mqtt.cfg";

  static void sanitizeConfig(MqttRuntimeConfig &config);
};

#endif
