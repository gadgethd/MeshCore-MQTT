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

  // Read header first to determine version
  struct { uint32_t magic; uint16_t version; } header;
  size_t hdr_read = file.read((uint8_t *)&header, sizeof(header));
  file.close();

  if (hdr_read < sizeof(header)) return false;
  if (header.magic != CONFIG_MAGIC) return false;

  // Re-read full file
  file = _fs->open(CONFIG_PATH, "r");
  if (!file) return false;

  if (header.version == 1) {
    PersistedMqttConfigV1 v1;
    size_t bytes_read = file.read((uint8_t *)&v1, sizeof(v1));
    file.close();
    if (bytes_read != sizeof(v1)) return false;
    return loadV1((const uint8_t *)&v1, bytes_read);
  }

  if (header.version == CONFIG_VERSION) {
    PersistedMqttConfigV2 v2;
    size_t bytes_read = file.read((uint8_t *)&v2, sizeof(v2));
    file.close();
    if (bytes_read != sizeof(v2)) return false;

    _shared = v2.shared;
    sanitizeShared(_shared);
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      _brokers[i] = v2.brokers[i];
      sanitizeBroker(_brokers[i]);
    }
    return true;
  }

  file.close();
  return false;
}

bool MqttSettingsStore::loadV1(const uint8_t *data, size_t len) {
  if (len < sizeof(PersistedMqttConfigV1)) return false;
  const PersistedMqttConfigV1 *v1 = (const PersistedMqttConfigV1 *)data;

  // Migrate shared fields
  StrHelper::strncpy(_shared.wifi_ssid, v1->config.wifi_ssid, sizeof(_shared.wifi_ssid));
  StrHelper::strncpy(_shared.wifi_pwd, v1->config.wifi_pwd, sizeof(_shared.wifi_pwd));
  StrHelper::strncpy(_shared.model, v1->config.model, sizeof(_shared.model));
  StrHelper::strncpy(_shared.client_version, v1->config.client_version, sizeof(_shared.client_version));
  sanitizeShared(_shared);

  // Migrate broker 0 fields
  StrHelper::strncpy(_brokers[0].uri, v1->config.uri, sizeof(_brokers[0].uri));
  StrHelper::strncpy(_brokers[0].username, v1->config.username, sizeof(_brokers[0].username));
  StrHelper::strncpy(_brokers[0].password, v1->config.password, sizeof(_brokers[0].password));
  StrHelper::strncpy(_brokers[0].topic_root, v1->config.topic_root, sizeof(_brokers[0].topic_root));
  StrHelper::strncpy(_brokers[0].iata, v1->config.iata, sizeof(_brokers[0].iata));
  _brokers[0].retain_status = v1->config.retain_status ? 1 : 0;
  _brokers[0].enabled = 1;
  sanitizeBroker(_brokers[0]);

  // Save as v2 format
  save();
  Serial.println("MQTT settings: migrated v1 -> v2");
  return true;
}

bool MqttSettingsStore::save() {
  if (_fs == nullptr) return false;

  PersistedMqttConfigV2 v2 = {};
  v2.magic = CONFIG_MAGIC;
  v2.version = CONFIG_VERSION;
  v2.broker_count = (uint8_t)brokerCount();
  v2.reserved = 0;
  v2.shared = _shared;
  sanitizeShared(v2.shared);
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    v2.brokers[i] = _brokers[i];
    sanitizeBroker(v2.brokers[i]);
  }

  File file = _fs->open(CONFIG_PATH, "w");
  if (!file) return false;

  size_t bytes_written = file.write((const uint8_t *)&v2, sizeof(v2));
  file.close();
  return bytes_written == sizeof(v2);
}

void MqttSettingsStore::resetToDefaults() {
  memset(&_shared, 0, sizeof(_shared));
  memset(_brokers, 0, sizeof(_brokers));

  StrHelper::strncpy(_shared.wifi_ssid, WIFI_SSID, sizeof(_shared.wifi_ssid));
  StrHelper::strncpy(_shared.wifi_pwd, WIFI_PWD, sizeof(_shared.wifi_pwd));
  StrHelper::strncpy(_shared.model, MQTT_MODEL, sizeof(_shared.model));
  StrHelper::strncpy(_shared.client_version, MQTT_CLIENT_VERSION, sizeof(_shared.client_version));

  // Broker 0 gets compile-time defaults and is enabled
  StrHelper::strncpy(_brokers[0].uri, MQTT_URI, sizeof(_brokers[0].uri));
  StrHelper::strncpy(_brokers[0].username, MQTT_USERNAME, sizeof(_brokers[0].username));
  StrHelper::strncpy(_brokers[0].password, MQTT_PASSWORD, sizeof(_brokers[0].password));
  StrHelper::strncpy(_brokers[0].topic_root, MQTT_TOPIC_ROOT, sizeof(_brokers[0].topic_root));
  StrHelper::strncpy(_brokers[0].iata, MQTT_IATA, sizeof(_brokers[0].iata));
  _brokers[0].retain_status = MQTT_RETAIN_STATUS ? 1 : 0;
  _brokers[0].enabled = 1;
}

int MqttSettingsStore::brokerCount() const {
  int count = 0;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_brokers[i].enabled) count++;
  }
  return count;
}

int MqttSettingsStore::parseKey(const char *key, const char **key_out) {
  // Check for "N." prefix where N is 1-6
  if (key[0] >= '1' && key[0] <= '0' + MQTT_MAX_BROKERS && key[1] == '.') {
    *key_out = key + 2;
    return key[0] - '1'; // 0-based index
  }
  *key_out = key;

  // Shared keys return -1
  if (strcmp(key, "wifi.ssid") == 0 || strcmp(key, "wifi.pass") == 0 ||
      strcmp(key, "model") == 0 || strcmp(key, "client.version") == 0) {
    return -1;
  }

  // Unindexed per-broker keys default to broker 0
  return 0;
}

bool MqttSettingsStore::getValue(const char *key, char *dest, size_t dest_size, bool mask_secret) const {
  if (key == nullptr || dest == nullptr || dest_size == 0) return false;

  const char *field;
  int idx = parseKey(key, &field);

  const char *value = nullptr;
  char masked[8];

  // Shared keys
  if (idx == -1) {
    if (strcmp(field, "wifi.ssid") == 0) {
      value = _shared.wifi_ssid;
    } else if (strcmp(field, "wifi.pass") == 0) {
      value = _shared.wifi_pwd;
    } else if (strcmp(field, "model") == 0) {
      value = _shared.model;
    } else if (strcmp(field, "client.version") == 0) {
      value = _shared.client_version;
    } else {
      return false;
    }

    if (mask_secret && strcmp(field, "wifi.pass") == 0) {
      value = value[0] ? "******" : "";
      StrHelper::strncpy(masked, value, sizeof(masked));
      value = masked;
    }

    StrHelper::strncpy(dest, value, dest_size);
    return true;
  }

  // Per-broker keys
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  const MqttBrokerConfig &b = _brokers[idx];

  if (strcmp(field, "uri") == 0) {
    value = b.uri;
  } else if (strcmp(field, "username") == 0) {
    value = b.username;
  } else if (strcmp(field, "password") == 0) {
    value = b.password;
  } else if (strcmp(field, "topic.root") == 0) {
    value = b.topic_root;
  } else if (strcmp(field, "iata") == 0) {
    value = b.iata;
  } else if (strcmp(field, "retain.status") == 0) {
    value = b.retain_status ? "1" : "0";
  } else if (strcmp(field, "enabled") == 0) {
    value = b.enabled ? "1" : "0";
  } else {
    return false;
  }

  if (mask_secret && strcmp(field, "password") == 0) {
    value = value[0] ? "******" : "";
    StrHelper::strncpy(masked, value, sizeof(masked));
    value = masked;
  }

  StrHelper::strncpy(dest, value, dest_size);
  return true;
}

bool MqttSettingsStore::setValue(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) return false;

  const char *field;
  int idx = parseKey(key, &field);

  // Shared keys
  if (idx == -1) {
    if (strcmp(field, "wifi.ssid") == 0) {
      StrHelper::strncpy(_shared.wifi_ssid, value, sizeof(_shared.wifi_ssid));
    } else if (strcmp(field, "wifi.pass") == 0) {
      StrHelper::strncpy(_shared.wifi_pwd, value, sizeof(_shared.wifi_pwd));
    } else if (strcmp(field, "model") == 0) {
      StrHelper::strncpy(_shared.model, value, sizeof(_shared.model));
    } else if (strcmp(field, "client.version") == 0) {
      StrHelper::strncpy(_shared.client_version, value, sizeof(_shared.client_version));
    } else {
      return false;
    }
    sanitizeShared(_shared);
    return true;
  }

  // Per-broker keys
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  MqttBrokerConfig &b = _brokers[idx];

  if (strcmp(field, "uri") == 0) {
    StrHelper::strncpy(b.uri, value, sizeof(b.uri));
  } else if (strcmp(field, "username") == 0) {
    StrHelper::strncpy(b.username, value, sizeof(b.username));
  } else if (strcmp(field, "password") == 0) {
    StrHelper::strncpy(b.password, value, sizeof(b.password));
  } else if (strcmp(field, "topic.root") == 0) {
    StrHelper::strncpy(b.topic_root, value, sizeof(b.topic_root));
  } else if (strcmp(field, "iata") == 0) {
    StrHelper::strncpy(b.iata, value, sizeof(b.iata));
  } else if (strcmp(field, "retain.status") == 0) {
    b.retain_status = (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
  } else if (strcmp(field, "enabled") == 0) {
    b.enabled = (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
  } else {
    return false;
  }

  sanitizeBroker(b);
  return true;
}

void MqttSettingsStore::sanitizeShared(MqttSharedConfig &cfg) {
  cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
  cfg.wifi_pwd[sizeof(cfg.wifi_pwd) - 1] = '\0';
  cfg.model[sizeof(cfg.model) - 1] = '\0';
  cfg.client_version[sizeof(cfg.client_version) - 1] = '\0';

  if (cfg.model[0] == '\0') {
    StrHelper::strncpy(cfg.model, MQTT_MODEL, sizeof(cfg.model));
  }
  if (cfg.client_version[0] == '\0') {
    StrHelper::strncpy(cfg.client_version, MQTT_CLIENT_VERSION, sizeof(cfg.client_version));
  }
}

void MqttSettingsStore::sanitizeBroker(MqttBrokerConfig &cfg) {
  cfg.uri[sizeof(cfg.uri) - 1] = '\0';
  cfg.username[sizeof(cfg.username) - 1] = '\0';
  cfg.password[sizeof(cfg.password) - 1] = '\0';
  cfg.topic_root[sizeof(cfg.topic_root) - 1] = '\0';
  cfg.iata[sizeof(cfg.iata) - 1] = '\0';

  if (cfg.topic_root[0] == '\0') {
    StrHelper::strncpy(cfg.topic_root, MQTT_TOPIC_ROOT, sizeof(cfg.topic_root));
  }
  if (cfg.iata[0] == '\0') {
    StrHelper::strncpy(cfg.iata, MQTT_IATA, sizeof(cfg.iata));
  }
  cfg.retain_status = cfg.retain_status ? 1 : 0;
  cfg.enabled = cfg.enabled ? 1 : 0;
}

#endif
