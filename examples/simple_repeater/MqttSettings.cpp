#include "MqttSettings.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <string.h>
#include <strings.h>

namespace {

struct LegacyMqttRuntimeConfig {
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

struct GlobalMultiBrokerRuntimeConfigV2 {
  char wifi_ssid[64];
  char wifi_pwd[64];
  char topic_root[32];
  char iata[16];
  char model[64];
  char client_version[64];
  uint8_t retain_status;
  MqttBrokerRuntimeConfig brokers[MQTT_MAX_BROKERS];
};

struct LegacyPersistedMqttConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  LegacyMqttRuntimeConfig config;
};

struct PersistedMqttConfigV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  GlobalMultiBrokerRuntimeConfigV2 config;
};

bool parseBrokerKey(const char *key, int &broker_index, const char *&field_name) {
  if (key == nullptr || memcmp(key, "broker", 6) != 0) return false;

  const char *index_ptr = key + 6;
  if (*index_ptr < '1' || *index_ptr > '5') return false;

  broker_index = *index_ptr - '1';
  if (broker_index < 0 || broker_index >= MQTT_MAX_BROKERS) return false;

  if (index_ptr[1] != '.') return false;
  field_name = index_ptr + 2;
  return field_name[0] != '\0';
}

void copyLegacyConfig(MqttRuntimeConfig &dest, const LegacyMqttRuntimeConfig &src) {
  StrHelper::strncpy(dest.wifi_ssid, src.wifi_ssid, sizeof(dest.wifi_ssid));
  StrHelper::strncpy(dest.wifi_pwd, src.wifi_pwd, sizeof(dest.wifi_pwd));
  StrHelper::strncpy(dest.brokers[0].topic_root, src.topic_root, sizeof(dest.brokers[0].topic_root));
  StrHelper::strncpy(dest.brokers[0].uri, src.uri, sizeof(dest.brokers[0].uri));
  StrHelper::strncpy(dest.brokers[0].username, src.username, sizeof(dest.brokers[0].username));
  StrHelper::strncpy(dest.brokers[0].password, src.password, sizeof(dest.brokers[0].password));
  StrHelper::strncpy(dest.brokers[0].iata, src.iata, sizeof(dest.brokers[0].iata));
  StrHelper::strncpy(dest.brokers[0].model, src.model, sizeof(dest.brokers[0].model));
  StrHelper::strncpy(dest.brokers[0].client_version, src.client_version, sizeof(dest.brokers[0].client_version));
  dest.brokers[0].retain_status = src.retain_status;
  dest.brokers[0].enabled = src.uri[0] != '\0' ? 1 : 0;
}

void copyV2Config(MqttRuntimeConfig &dest, const GlobalMultiBrokerRuntimeConfigV2 &src) {
  StrHelper::strncpy(dest.wifi_ssid, src.wifi_ssid, sizeof(dest.wifi_ssid));
  StrHelper::strncpy(dest.wifi_pwd, src.wifi_pwd, sizeof(dest.wifi_pwd));
  memcpy(dest.brokers, src.brokers, sizeof(dest.brokers));
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (dest.brokers[i].topic_root[0] == '\0') {
      StrHelper::strncpy(dest.brokers[i].topic_root, src.topic_root, sizeof(dest.brokers[i].topic_root));
    }
    if (dest.brokers[i].iata[0] == '\0') {
      StrHelper::strncpy(dest.brokers[i].iata, src.iata, sizeof(dest.brokers[i].iata));
    }
    if (dest.brokers[i].model[0] == '\0') {
      StrHelper::strncpy(dest.brokers[i].model, src.model, sizeof(dest.brokers[i].model));
    }
    if (dest.brokers[i].client_version[0] == '\0') {
      StrHelper::strncpy(dest.brokers[i].client_version, src.client_version, sizeof(dest.brokers[i].client_version));
    }
    if (dest.brokers[i].retain_status == 0 && src.retain_status != 0) {
      dest.brokers[i].retain_status = src.retain_status;
    }
  }
}

} // namespace

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

  size_t file_size = file.size();
  if (file_size == sizeof(PersistedMqttConfig)) {
    PersistedMqttConfig persisted;
    size_t bytes_read = file.read((uint8_t *)&persisted, sizeof(persisted));
    file.close();

    if (bytes_read != sizeof(persisted)) return false;
    if (persisted.magic != CONFIG_MAGIC || persisted.version != CONFIG_VERSION) return false;

    _config = persisted.config;
    sanitizeConfig(_config);
    return true;
  }

  if (file_size == sizeof(PersistedMqttConfigV2)) {
    PersistedMqttConfigV2 persisted;
    size_t bytes_read = file.read((uint8_t *)&persisted, sizeof(persisted));
    file.close();

    if (bytes_read != sizeof(persisted)) return false;
    if (persisted.magic != CONFIG_MAGIC || persisted.version != 2) return false;

    copyV2Config(_config, persisted.config);
    sanitizeConfig(_config);
    return true;
  }

  if (file_size == sizeof(LegacyPersistedMqttConfig)) {
    LegacyPersistedMqttConfig legacy;
    size_t bytes_read = file.read((uint8_t *)&legacy, sizeof(legacy));
    file.close();

    if (bytes_read != sizeof(legacy)) return false;
    if (legacy.magic != CONFIG_MAGIC || legacy.version != 1) return false;

    copyLegacyConfig(_config, legacy.config);
    sanitizeConfig(_config);
    return true;
  }

  file.close();
  return false;
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
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    StrHelper::strncpy(_config.brokers[i].topic_root, MQTT_TOPIC_ROOT, sizeof(_config.brokers[i].topic_root));
    StrHelper::strncpy(_config.brokers[i].iata, MQTT_IATA, sizeof(_config.brokers[i].iata));
    StrHelper::strncpy(_config.brokers[i].model, MQTT_MODEL, sizeof(_config.brokers[i].model));
    StrHelper::strncpy(_config.brokers[i].client_version, MQTT_CLIENT_VERSION, sizeof(_config.brokers[i].client_version));
    _config.brokers[i].retain_status = MQTT_RETAIN_STATUS ? 1 : 0;
  }
  StrHelper::strncpy(_config.brokers[0].uri, MQTT_URI, sizeof(_config.brokers[0].uri));
  StrHelper::strncpy(_config.brokers[0].username, MQTT_USERNAME, sizeof(_config.brokers[0].username));
  StrHelper::strncpy(_config.brokers[0].password, MQTT_PASSWORD, sizeof(_config.brokers[0].password));
  _config.brokers[0].enabled = MQTT_URI[0] != '\0' ? 1 : 0;
}

bool MqttSettingsStore::getValue(const char *key, char *dest, size_t dest_size, bool mask_secret) const {
  if (key == nullptr || dest == nullptr || dest_size == 0) return false;

  const char *value = nullptr;
  char masked[8];
  int broker_index = -1;
  const char *field_name = nullptr;

  if (strcmp(key, "wifi.ssid") == 0) {
    value = _config.wifi_ssid;
  } else if (strcmp(key, "wifi.pass") == 0) {
    value = _config.wifi_pwd;
  } else if (strcmp(key, "topic.root") == 0) {
    value = _config.brokers[0].topic_root;
  } else if (strcmp(key, "uri") == 0) {
    value = _config.brokers[0].uri;
  } else if (strcmp(key, "username") == 0) {
    value = _config.brokers[0].username;
  } else if (strcmp(key, "password") == 0) {
    value = _config.brokers[0].password;
  } else if (strcmp(key, "iata") == 0) {
    value = _config.brokers[0].iata;
  } else if (strcmp(key, "model") == 0) {
    value = _config.brokers[0].model;
  } else if (strcmp(key, "client.version") == 0) {
    value = _config.brokers[0].client_version;
  } else if (strcmp(key, "retain.status") == 0) {
    value = _config.brokers[0].retain_status ? "1" : "0";
  } else if (parseBrokerKey(key, broker_index, field_name)) {
    if (strcmp(field_name, "topic.root") == 0) {
      value = _config.brokers[broker_index].topic_root;
    } else if (strcmp(field_name, "uri") == 0) {
      value = _config.brokers[broker_index].uri;
    } else if (strcmp(field_name, "username") == 0) {
      value = _config.brokers[broker_index].username;
    } else if (strcmp(field_name, "password") == 0) {
      value = _config.brokers[broker_index].password;
    } else if (strcmp(field_name, "iata") == 0) {
      value = _config.brokers[broker_index].iata;
    } else if (strcmp(field_name, "model") == 0) {
      value = _config.brokers[broker_index].model;
    } else if (strcmp(field_name, "client.version") == 0) {
      value = _config.brokers[broker_index].client_version;
    } else if (strcmp(field_name, "retain.status") == 0) {
      value = _config.brokers[broker_index].retain_status ? "1" : "0";
    } else if (strcmp(field_name, "enabled") == 0) {
      value = _config.brokers[broker_index].enabled ? "1" : "0";
    } else {
      return false;
    }
  } else {
    return false;
  }

  if (mask_secret &&
      ((strcmp(key, "wifi.pass") == 0) || (strcmp(key, "password") == 0) ||
       (field_name != nullptr && strcmp(field_name, "password") == 0))) {
    value = value[0] ? "******" : "";
    StrHelper::strncpy(masked, value, sizeof(masked));
    value = masked;
  }

  StrHelper::strncpy(dest, value, dest_size);
  return true;
}

bool MqttSettingsStore::setValue(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) return false;
  int broker_index = -1;
  const char *field_name = nullptr;

  if (strcmp(key, "wifi.ssid") == 0) {
    StrHelper::strncpy(_config.wifi_ssid, value, sizeof(_config.wifi_ssid));
  } else if (strcmp(key, "wifi.pass") == 0) {
    StrHelper::strncpy(_config.wifi_pwd, value, sizeof(_config.wifi_pwd));
  } else if (strcmp(key, "topic.root") == 0) {
    StrHelper::strncpy(_config.brokers[0].topic_root, value, sizeof(_config.brokers[0].topic_root));
  } else if (strcmp(key, "uri") == 0) {
    StrHelper::strncpy(_config.brokers[0].uri, value, sizeof(_config.brokers[0].uri));
    _config.brokers[0].enabled = value[0] != '\0' ? 1 : 0;
  } else if (strcmp(key, "username") == 0) {
    StrHelper::strncpy(_config.brokers[0].username, value, sizeof(_config.brokers[0].username));
  } else if (strcmp(key, "password") == 0) {
    StrHelper::strncpy(_config.brokers[0].password, value, sizeof(_config.brokers[0].password));
  } else if (strcmp(key, "iata") == 0) {
    StrHelper::strncpy(_config.brokers[0].iata, value, sizeof(_config.brokers[0].iata));
  } else if (strcmp(key, "model") == 0) {
    StrHelper::strncpy(_config.brokers[0].model, value, sizeof(_config.brokers[0].model));
  } else if (strcmp(key, "client.version") == 0) {
    StrHelper::strncpy(_config.brokers[0].client_version, value, sizeof(_config.brokers[0].client_version));
  } else if (strcmp(key, "retain.status") == 0) {
    _config.brokers[0].retain_status = (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
  } else if (parseBrokerKey(key, broker_index, field_name)) {
    if (strcmp(field_name, "topic.root") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].topic_root, value, sizeof(_config.brokers[broker_index].topic_root));
    } else if (strcmp(field_name, "uri") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].uri, value, sizeof(_config.brokers[broker_index].uri));
      _config.brokers[broker_index].enabled = value[0] != '\0' ? 1 : 0;
    } else if (strcmp(field_name, "username") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].username, value, sizeof(_config.brokers[broker_index].username));
    } else if (strcmp(field_name, "password") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].password, value, sizeof(_config.brokers[broker_index].password));
    } else if (strcmp(field_name, "iata") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].iata, value, sizeof(_config.brokers[broker_index].iata));
    } else if (strcmp(field_name, "model") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].model, value, sizeof(_config.brokers[broker_index].model));
    } else if (strcmp(field_name, "client.version") == 0) {
      StrHelper::strncpy(_config.brokers[broker_index].client_version, value, sizeof(_config.brokers[broker_index].client_version));
    } else if (strcmp(field_name, "retain.status") == 0) {
      _config.brokers[broker_index].retain_status =
          (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
    } else if (strcmp(field_name, "enabled") == 0) {
      _config.brokers[broker_index].enabled =
          (strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0) ? 0 : 1;
    } else {
      return false;
    }
  } else {
    return false;
  }

  sanitizeConfig(_config);
  return true;
}

void MqttSettingsStore::sanitizeConfig(MqttRuntimeConfig &config) {
  config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
  config.wifi_pwd[sizeof(config.wifi_pwd) - 1] = '\0';
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    config.brokers[i].topic_root[sizeof(config.brokers[i].topic_root) - 1] = '\0';
    config.brokers[i].uri[sizeof(config.brokers[i].uri) - 1] = '\0';
    config.brokers[i].username[sizeof(config.brokers[i].username) - 1] = '\0';
    config.brokers[i].password[sizeof(config.brokers[i].password) - 1] = '\0';
    config.brokers[i].iata[sizeof(config.brokers[i].iata) - 1] = '\0';
    config.brokers[i].model[sizeof(config.brokers[i].model) - 1] = '\0';
    config.brokers[i].client_version[sizeof(config.brokers[i].client_version) - 1] = '\0';
    config.brokers[i].retain_status = config.brokers[i].retain_status ? 1 : 0;
    config.brokers[i].enabled = config.brokers[i].enabled ? 1 : 0;
    if (config.brokers[i].topic_root[0] == '\0') {
      StrHelper::strncpy(config.brokers[i].topic_root, MQTT_TOPIC_ROOT, sizeof(config.brokers[i].topic_root));
    }
    if (config.brokers[i].iata[0] == '\0') {
      StrHelper::strncpy(config.brokers[i].iata, MQTT_IATA, sizeof(config.brokers[i].iata));
    }
    if (config.brokers[i].model[0] == '\0') {
      StrHelper::strncpy(config.brokers[i].model, MQTT_MODEL, sizeof(config.brokers[i].model));
    }
    if (config.brokers[i].client_version[0] == '\0') {
      StrHelper::strncpy(config.brokers[i].client_version, MQTT_CLIENT_VERSION, sizeof(config.brokers[i].client_version));
    }
    if (config.brokers[i].uri[0] == '\0') {
      config.brokers[i].enabled = 0;
    }
  }
}

#endif
