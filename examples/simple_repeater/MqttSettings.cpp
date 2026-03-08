#include "MqttSettings.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <string.h>
#include <strings.h>

MqttSettingsStore::MqttSettingsStore() : _fs(nullptr) {
  resetToDefaults();
}

void MqttSettingsStore::begin(FILESYSTEM *fs) {
  _fs = fs;
  load();
}

bool MqttSettingsStore::load() {
  resetToDefaults();
  if (_fs == nullptr) return false;

  File file = _fs->open(CONFIG_PATH, "r");
  if (!file) return false;

  PersistedMqttConfig persisted;
  size_t bytes_read = file.read((uint8_t *)&persisted, sizeof(persisted));
  file.close();

  if (bytes_read != sizeof(persisted)) return false;
  if (persisted.magic != CONFIG_MAGIC || persisted.version != CONFIG_VERSION) return false;

  _config = persisted.config;
  sanitizeConfig(_config);
  return true;
}

bool MqttSettingsStore::save() {
  if (_fs == nullptr) return false;

  PersistedMqttConfig persisted = {};
  persisted.magic = CONFIG_MAGIC;
  persisted.version = CONFIG_VERSION;
  persisted.config = _config;
  sanitizeConfig(persisted.config);

  File file = _fs->open(CONFIG_PATH, "w");
  if (!file) return false;

  size_t bytes_written = file.write((const uint8_t *)&persisted, sizeof(persisted));
  file.close();
  return bytes_written == sizeof(persisted);
}

void MqttSettingsStore::resetToDefaults() {
  memset(&_config, 0, sizeof(_config));
  StrHelper::strncpy(_config.wifi_ssid, WIFI_SSID, sizeof(_config.wifi_ssid));
  StrHelper::strncpy(_config.wifi_pwd, WIFI_PWD, sizeof(_config.wifi_pwd));
  StrHelper::strncpy(_config.topic_root, MQTT_TOPIC_ROOT, sizeof(_config.topic_root));
  StrHelper::strncpy(_config.uri, MQTT_URI, sizeof(_config.uri));
  StrHelper::strncpy(_config.username, MQTT_USERNAME, sizeof(_config.username));
  StrHelper::strncpy(_config.password, MQTT_PASSWORD, sizeof(_config.password));
  StrHelper::strncpy(_config.iata, MQTT_IATA, sizeof(_config.iata));
  StrHelper::strncpy(_config.model, MQTT_MODEL, sizeof(_config.model));
  StrHelper::strncpy(_config.client_version, MQTT_CLIENT_VERSION, sizeof(_config.client_version));
  _config.retain_status = MQTT_RETAIN_STATUS ? 1 : 0;
}

bool MqttSettingsStore::getValue(const char *key, char *dest, size_t dest_size, bool mask_secret) const {
  if (key == nullptr || dest == nullptr || dest_size == 0) return false;

  const char *value = nullptr;
  char masked[8];

  if (strcmp(key, "wifi.ssid") == 0) {
    value = _config.wifi_ssid;
  } else if (strcmp(key, "wifi.pass") == 0) {
    value = _config.wifi_pwd;
  } else if (strcmp(key, "topic.root") == 0) {
    value = _config.topic_root;
  } else if (strcmp(key, "uri") == 0) {
    value = _config.uri;
  } else if (strcmp(key, "username") == 0) {
    value = _config.username;
  } else if (strcmp(key, "password") == 0) {
    value = _config.password;
  } else if (strcmp(key, "iata") == 0) {
    value = _config.iata;
  } else if (strcmp(key, "model") == 0) {
    value = _config.model;
  } else if (strcmp(key, "client.version") == 0) {
    value = _config.client_version;
  } else if (strcmp(key, "retain.status") == 0) {
    value = _config.retain_status ? "1" : "0";
  } else {
    return false;
  }

  if (mask_secret && ((strcmp(key, "wifi.pass") == 0) || (strcmp(key, "password") == 0))) {
    value = value[0] ? "******" : "";
    StrHelper::strncpy(masked, value, sizeof(masked));
    value = masked;
  }

  StrHelper::strncpy(dest, value, dest_size);
  return true;
}

bool MqttSettingsStore::setValue(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) return false;

  if (strcmp(key, "wifi.ssid") == 0) {
    StrHelper::strncpy(_config.wifi_ssid, value, sizeof(_config.wifi_ssid));
  } else if (strcmp(key, "wifi.pass") == 0) {
    StrHelper::strncpy(_config.wifi_pwd, value, sizeof(_config.wifi_pwd));
  } else if (strcmp(key, "topic.root") == 0) {
    StrHelper::strncpy(_config.topic_root, value, sizeof(_config.topic_root));
  } else if (strcmp(key, "uri") == 0) {
    StrHelper::strncpy(_config.uri, value, sizeof(_config.uri));
  } else if (strcmp(key, "username") == 0) {
    StrHelper::strncpy(_config.username, value, sizeof(_config.username));
  } else if (strcmp(key, "password") == 0) {
    StrHelper::strncpy(_config.password, value, sizeof(_config.password));
  } else if (strcmp(key, "iata") == 0) {
    StrHelper::strncpy(_config.iata, value, sizeof(_config.iata));
  } else if (strcmp(key, "model") == 0) {
    StrHelper::strncpy(_config.model, value, sizeof(_config.model));
  } else if (strcmp(key, "client.version") == 0) {
    StrHelper::strncpy(_config.client_version, value, sizeof(_config.client_version));
  } else if (strcmp(key, "retain.status") == 0) {
    _config.retain_status = (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
  } else {
    return false;
  }

  sanitizeConfig(_config);
  return true;
}

void MqttSettingsStore::sanitizeConfig(MqttRuntimeConfig &config) {
  config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
  config.wifi_pwd[sizeof(config.wifi_pwd) - 1] = '\0';
  config.topic_root[sizeof(config.topic_root) - 1] = '\0';
  config.uri[sizeof(config.uri) - 1] = '\0';
  config.username[sizeof(config.username) - 1] = '\0';
  config.password[sizeof(config.password) - 1] = '\0';
  config.iata[sizeof(config.iata) - 1] = '\0';
  config.model[sizeof(config.model) - 1] = '\0';
  config.client_version[sizeof(config.client_version) - 1] = '\0';

  if (config.topic_root[0] == '\0') {
    StrHelper::strncpy(config.topic_root, MQTT_TOPIC_ROOT, sizeof(config.topic_root));
  }
  if (config.iata[0] == '\0') {
    StrHelper::strncpy(config.iata, MQTT_IATA, sizeof(config.iata));
  }
  if (config.model[0] == '\0') {
    StrHelper::strncpy(config.model, MQTT_MODEL, sizeof(config.model));
  }
  if (config.client_version[0] == '\0') {
    StrHelper::strncpy(config.client_version, MQTT_CLIENT_VERSION, sizeof(config.client_version));
  }
  config.retain_status = config.retain_status ? 1 : 0;
}

#endif
