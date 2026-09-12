#include "MqttSettings.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <nvs.h>
#include <nvs_flash.h>
#include <helpers/UTF8Helpers.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

namespace {

constexpr size_t MQTT_TOPIC_PATH_BUFFER_SIZE = 384;
constexpr size_t MQTT_NEIGHBORS_SUFFIX_LENGTH = sizeof("/neighbors") - 1;
constexpr size_t MQTT_MAX_TOPIC_NAME_BYTES = 65535;
constexpr size_t MQTT_PUBLIC_KEY_HEX_LENGTH = 64;

bool validMqttTopicText(const char *text) {
  if (text == nullptr) return false;

  const size_t len = strlen(text);
  if (mesh::validUtf8PrefixLength(text, len) != len) return false;

  for (size_t i = 0; i < len; i++) {
    const uint8_t byte = static_cast<uint8_t>(text[i]);
    if (byte < 0x20 || byte == 0x7F || byte == '+' || byte == '#') return false;
    if (byte == 0xC2 && i + 1 < len) {
      const uint8_t next = static_cast<uint8_t>(text[i + 1]);
      if (next >= 0x80 && next <= 0x9F) return false;
    }
  }
  return true;
}

size_t expandedTopicLength(const char *topic_root, const char *iata) {
  size_t length = 0;
  for (size_t i = 0; topic_root[i] != '\0';) {
    const char *token = nullptr;
    size_t replacement_length = 0;
    if (strncmp(topic_root + i, "{IATA}", 6) == 0) {
      token = "{IATA}";
      replacement_length = strlen(iata);
    } else if (strncmp(topic_root + i, "<IATA>", 6) == 0) {
      token = "<IATA>";
      replacement_length = strlen(iata);
    } else if (strncmp(topic_root + i, "{PUBLIC_KEY}", 12) == 0) {
      token = "{PUBLIC_KEY}";
      replacement_length = MQTT_PUBLIC_KEY_HEX_LENGTH;
    } else if (strncmp(topic_root + i, "<PUBLIC_KEY>", 12) == 0) {
      token = "<PUBLIC_KEY>";
      replacement_length = MQTT_PUBLIC_KEY_HEX_LENGTH;
    }

    if (token != nullptr) {
      length += replacement_length;
      i += strlen(token);
    } else {
      length++;
      i++;
    }
  }
  return length;
}

bool validBrokerTopicConfig(const MqttBrokerConfig &cfg) {
  if (!validMqttTopicText(cfg.topic_root) || !validMqttTopicText(cfg.iata)) return false;

  const size_t expanded_length = expandedTopicLength(cfg.topic_root, cfg.iata);
  if (expanded_length > MQTT_MAX_TOPIC_NAME_BYTES) return false;
  return expanded_length + MQTT_NEIGHBORS_SUFFIX_LENGTH < MQTT_TOPIC_PATH_BUFFER_SIZE;
}

} // namespace

MqttSettingsStore::MqttSettingsStore()
    : _fs(nullptr), _boot_count(0), _prefs_write_hold(false),
      _loaded_source(mqtt_prefs::RecoverySource::None) {
  resetToDefaults();
}

// A fully erased (or corrupted) NVS partition must be initialized before the
// first nvs_open. The Arduino core's initArduino only warns on failure, so a
// blank partition panics at the first nvs_open. This mirrors the standard
// ESP-IDF recovery pattern.
static void ensureNvsReady() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
}

// Remove a file only when it exists: LittleFS/VFS logs an error for removing
// a missing path, which used to spam the serial log on every settings save.
static bool removeIfExists(FILESYSTEM *fs, const char *path) {
  if (fs == nullptr || path == nullptr || !fs->exists(path)) return true;
  return fs->remove(path);
}

void MqttSettingsStore::begin(FILESYSTEM *fs) {
  _fs = fs;
  loadBootCount();
  load();
}

void MqttSettingsStore::loadBootCount() {
  ensureNvsReady();
  nvs_handle_t handle;
  if (nvs_open("mqtt", NVS_READONLY, &handle) == ESP_OK) {
    uint32_t value = 0;
    if (nvs_get_u32(handle, "boot_count", &value) == ESP_OK) {
      _boot_count = value;
    }
    nvs_close(handle);
  }
}

uint32_t MqttSettingsStore::incrementBootCount() {
  if (_boot_count != UINT32_MAX) {
    _boot_count++;
  }
  saveBootCount();
  return _boot_count;
}

uint16_t MqttSettingsStore::configVersion() {
  return CONFIG_VERSION;
}

bool MqttSettingsStore::load() {
  resetToDefaults();
  _loaded_source = mqtt_prefs::RecoverySource::None;
  if (_fs == nullptr) return false;

  const auto tryPath = [this](const char *path, mqtt_prefs::RecoverySource source,
                              bool migrate_legacy) {
    resetToDefaults();
    _loaded_source = source;
    const LoadResult result = loadPath(path, migrate_legacy);
    if (result != LoadResult::Loaded) {
      _loaded_source = mqtt_prefs::RecoverySource::None;
    }
    return result;
  };

  const LoadResult primary_result = tryPath(CONFIG_PATH, mqtt_prefs::RecoverySource::Primary, true);
  if (primary_result == LoadResult::Loaded) return true;
  if (primary_result == LoadResult::MigrationFailed) return false;

  // A newer/opaque primary owns its name. Preserve the existing write-hold
  // semantics and do not let a temp image overwrite it. A known-invalid
  // primary, however, may be recovered from a complete verified temp first.
  if (primary_result != LoadResult::Preserve) {
    const LoadResult temp_result = tryPath(CONFIG_TEMP_PATH, mqtt_prefs::RecoverySource::Temp, false);
    if (temp_result == LoadResult::Loaded) {
      if (promoteRecoveredTemp()) {
        _loaded_source = mqtt_prefs::RecoverySource::Primary;
        return true;
      }
      Serial.println("MQTT settings: verified temp promotion failed; retaining temp for retry");
      _loaded_source = mqtt_prefs::RecoverySource::None;
    } else if (temp_result == LoadResult::MigrationFailed) {
      return false;
    }
  }

  const LoadResult backup_result = tryPath(CONFIG_BACKUP_PATH, mqtt_prefs::RecoverySource::Backup, true);
  if (backup_result == LoadResult::Loaded) return true;
  if (backup_result == LoadResult::MigrationFailed) return false;

  if (_prefs_write_hold) {
    // Keep the hold latched even when neither fallback could be decoded.
    resetToDefaults();
    return false;
  }

  resetToDefaults();
  _loaded_source = mqtt_prefs::RecoverySource::None;
  return false;
}

MqttSettingsStore::LoadResult MqttSettingsStore::loadPath(const char *path, bool migrate_legacy) {
  if (_fs == nullptr || path == nullptr) return LoadResult::Invalid;

  File file = _fs->open(path, "r");
  if (!file) return _fs->exists(path) ? LoadResult::Invalid : LoadResult::Missing;

  // Read the header first to determine the stored version (single-sourced
  // through the codec's classifier).
  uint8_t hdr_bytes[sizeof(mqtt_prefs::Header)];
  size_t hdr_read = file.read(hdr_bytes, sizeof(hdr_bytes));
  file.close();

  if (hdr_read < sizeof(hdr_bytes)) return LoadResult::Invalid;
  const mqtt_prefs::Kind kind = mqtt_prefs::classify(hdr_bytes, hdr_read);
  if (kind == mqtt_prefs::Kind::BadMagic) return LoadResult::Invalid;
  uint16_t stored_version = 0;
  for (int i = 0; i < 2; i++) stored_version |= (uint16_t)hdr_bytes[4 + i] << (8 * i);

  // Re-read full file
  file = _fs->open(path, "r");
  if (!file) return LoadResult::Invalid;

  if (kind == mqtt_prefs::Kind::V1) {
    if (!migrate_legacy) { file.close(); return LoadResult::Invalid; }
    // Legacy layout: read through a heap copy — the legacy structs are
    // hundreds of bytes to multi-KB and must never be placed on the stack
    // (that is exactly the overflow fixed here).
    PersistedMqttConfigV1 *v1 = (PersistedMqttConfigV1 *)malloc(sizeof(PersistedMqttConfigV1));
    if (v1 == nullptr) { file.close(); return LoadResult::Invalid; }
    size_t bytes_read = file.read((uint8_t *)v1, sizeof(*v1));
    file.close();
    if (bytes_read != sizeof(*v1)) { free(v1); return LoadResult::Invalid; }
    const bool ok = loadV1((const uint8_t *)v1, bytes_read);
    free(v1);
    return ok ? LoadResult::Loaded : LoadResult::MigrationFailed;
  }

  if (kind == mqtt_prefs::Kind::V2) {
    if (!migrate_legacy) { file.close(); return LoadResult::Invalid; }
    PersistedMqttConfigV2 *v2 = (PersistedMqttConfigV2 *)malloc(sizeof(PersistedMqttConfigV2));
    if (v2 == nullptr) { file.close(); return LoadResult::Invalid; }
    size_t bytes_read = file.read((uint8_t *)v2, sizeof(*v2));
    file.close();
    if (bytes_read != sizeof(*v2)) { free(v2); return LoadResult::Invalid; }
    const bool ok = loadV2((const uint8_t *)v2, bytes_read);
    free(v2);
    return ok ? LoadResult::Loaded : LoadResult::MigrationFailed;
  }

  if (kind == mqtt_prefs::Kind::V3) {
    if (!migrate_legacy) { file.close(); return LoadResult::Invalid; }
    PersistedMqttConfigV3 *v3 = (PersistedMqttConfigV3 *)malloc(sizeof(PersistedMqttConfigV3));
    if (v3 == nullptr) { file.close(); return LoadResult::Invalid; }
    size_t bytes_read = file.read((uint8_t *)v3, sizeof(*v3));
    file.close();
    if (bytes_read != sizeof(*v3)) { free(v3); return LoadResult::Invalid; }

    _shared = v3->shared;
    sanitizeShared(_shared);
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      mqtt_prefs::mapBrokerFromV3(v3->brokers[i], _brokers[i], MQTT_NEIGHBOR_INTERVAL_SECS);
      sanitizeBroker(_brokers[i]);
    }
    free(v3);
    if (!save()) {
      _prefs_write_hold = true;
      Serial.println("MQTT settings: v3 migration save failed; holding settings writes");
      return LoadResult::MigrationFailed;
    }
    Serial.println("MQTT settings: migrated v3 -> v4");
    return LoadResult::Loaded;
  }

  if (kind == mqtt_prefs::Kind::V4) {
    // Read the current layout field-by-field into the members. Serializing
    // through the members (never a full-image stack copy) is what keeps this
    // path inside the 8 KB loopTask stack.
    struct V4Header { uint32_t magic; uint16_t version; uint8_t broker_count; uint8_t reserved; };
    V4Header full_header;
    size_t bytes_read = file.read((uint8_t *)&full_header, sizeof(full_header));
    if (bytes_read != sizeof(full_header)) { file.close(); return LoadResult::Invalid; }

    bytes_read = file.read((uint8_t *)&_shared, sizeof(_shared));
    if (bytes_read != sizeof(_shared)) { file.close(); return LoadResult::Invalid; }
    sanitizeShared(_shared);

    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      bytes_read = file.read((uint8_t *)&_brokers[i], sizeof(_brokers[i]));
      if (bytes_read != sizeof(_brokers[i])) { file.close(); return LoadResult::Invalid; }
      sanitizeBroker(_brokers[i]);
    }
    file.close();
    return LoadResult::Loaded;
  }

  // A file written by a newer firmware that this build does not recognize.
  // Leave it untouched and block saves (write-hold) so a downgraded build
  // cannot clobber the newer configuration; `mqtt reset` clears the hold.
  _prefs_write_hold = true;
  Serial.printf("MQTT settings: config version %u not recognized; leaving file untouched (saves disabled until 'mqtt reset')\n",
                (unsigned int)stored_version);
  file.close();
  return LoadResult::Preserve;
}

bool MqttSettingsStore::loadV1(const uint8_t *data, size_t len) {
  if (len < sizeof(PersistedMqttConfigV1)) return false;
  const PersistedMqttConfigV1 *v1 = (const PersistedMqttConfigV1 *)data;

  // Migrate through the codec's field mapping (host-tested against fixtures).
  mqtt_prefs::mapSharedFromV1(*v1, _shared);
  sanitizeShared(_shared);

  mqtt_prefs::mapBrokerFromV1(v1->config, _brokers[0]);
  sanitizeBroker(_brokers[0]);

  // Save as v4 format
  if (!save()) {
    _prefs_write_hold = true;
    Serial.println("MQTT settings: v1 migration save failed; holding settings writes");
    return false;
  }
  Serial.println("MQTT settings: migrated v1 -> v4");
  return true;
}

bool MqttSettingsStore::loadV2(const uint8_t *data, size_t len) {
  if (len < sizeof(PersistedMqttConfigV2)) return false;
  const PersistedMqttConfigV2 *v2 = (const PersistedMqttConfigV2 *)data;

  _shared = v2->shared;
  sanitizeShared(_shared);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    mqtt_prefs::mapBrokerFromV2(v2->brokers[i], _brokers[i]);
    sanitizeBroker(_brokers[i]);
  }

  // Save as v4 format
  if (!save()) {
    _prefs_write_hold = true;
    Serial.println("MQTT settings: v2 migration save failed; holding settings writes");
    return false;
  }
  Serial.println("MQTT settings: migrated v2 -> v4");
  return true;
}

bool MqttSettingsStore::save() {
  return saveConfigFile();
}

bool MqttSettingsStore::saveBootCount() {
  ensureNvsReady();
  nvs_handle_t handle;
  if (nvs_open("mqtt", NVS_READWRITE, &handle) != ESP_OK) return false;
  bool saved = nvs_set_u32(handle, "boot_count", _boot_count) == ESP_OK;
  if (saved) {
    saved = nvs_commit(handle) == ESP_OK;
  }
  nvs_close(handle);
  return saved;
}

bool MqttSettingsStore::saveConfigFile() {
  if (_fs == nullptr) return false;
  if (_prefs_write_hold) {
    Serial.println("MQTT settings: save blocked (loaded file is from a newer firmware; 'mqtt reset' to adopt this build)");
    return false;
  }

  // Pin the on-disk layout this function must serialize byte-identically.
  static_assert(sizeof(MqttSharedConfig) == 256, "settings layout drift");
  static_assert(sizeof(MqttBrokerConfig) == 536, "settings layout drift");
  static_assert(sizeof(PersistedMqttConfigV4) == 8 + sizeof(MqttSharedConfig) + MQTT_MAX_BROKERS * sizeof(MqttBrokerConfig),
                "settings file layout drift");

  // Serialize piece-by-piece. Composing the full ~3.4 KB V4 image on the
  // stack overflowed the 8 KB loopTask stack and rebooted the node on every
  // `set` (decoded: Stack canary watchpoint, saveConfigFile -> loopTask).
  struct V4Header { uint32_t magic; uint16_t version; uint8_t broker_count; uint8_t reserved; };
  V4Header header;
  header.magic = CONFIG_MAGIC;
  header.version = CONFIG_VERSION;
  header.broker_count = (uint8_t)brokerCount();
  header.reserved = 0;

  if (!removeIfExists(_fs, CONFIG_TEMP_PATH)) {
    Serial.println("MQTT settings: could not replace existing temporary file");
    return false;
  }
  File file = _fs->open(CONFIG_TEMP_PATH, "w");
  if (!file) return false;
  bool write_ok = file.write((const uint8_t *)&header, sizeof(header)) == sizeof(header);
  if (write_ok) {
    MqttSharedConfig shared = _shared;
    sanitizeShared(shared);
    write_ok = file.write((const uint8_t *)&shared, sizeof(shared)) == sizeof(shared);
  }
  for (int i = 0; i < MQTT_MAX_BROKERS && write_ok; i++) {
    MqttBrokerConfig broker = _brokers[i];
    sanitizeBroker(broker);
    write_ok = file.write((const uint8_t *)&broker, sizeof(broker)) == sizeof(broker);
  }
  file.close();
  if (!write_ok) {
    if (!removeIfExists(_fs, CONFIG_TEMP_PATH)) {
      Serial.println("MQTT settings: failed write left an unusable temporary file");
    }
    return false;
  }

  // Read the temporary file back, piece-by-piece, before it can replace the
  // last-known-good file.
  file = _fs->open(CONFIG_TEMP_PATH, "r");
  if (!file) {
    return false;
  }
  bool read_ok = true;
  V4Header read_header;
  read_ok = file.read((uint8_t *)&read_header, sizeof(read_header)) == sizeof(read_header) &&
            read_header.magic == CONFIG_MAGIC && read_header.version == CONFIG_VERSION;
  if (read_ok) {
    MqttSharedConfig shared = _shared;
    sanitizeShared(shared);
    MqttSharedConfig read_shared;
    read_ok = file.read((uint8_t *)&read_shared, sizeof(read_shared)) == sizeof(read_shared) &&
              memcmp(&read_shared, &shared, sizeof(shared)) == 0;
    for (int i = 0; i < MQTT_MAX_BROKERS && read_ok; i++) {
      MqttBrokerConfig broker = _brokers[i];
      sanitizeBroker(broker);
      MqttBrokerConfig read_broker;
      read_ok = file.read((uint8_t *)&read_broker, sizeof(read_broker)) == sizeof(read_broker) &&
                memcmp(&read_broker, &broker, sizeof(broker)) == 0;
    }
  }
  file.close();
  if (!read_ok) {
    if (!removeIfExists(_fs, CONFIG_TEMP_PATH)) {
      Serial.println("MQTT settings: failed verification left an unusable temporary file");
    }
    return false;
  }

  const auto exists = [this](const char *path) { return _fs->exists(path); };
  const auto remove = [this](const char *path) { return _fs->remove(path); };
  const auto rename = [this](const char *from, const char *to) {
    return _fs->rename(from, to);
  };
  const mqtt_prefs::RecoveryCommitResult result = mqtt_prefs::commitVerifiedTemp(
      _loaded_source, CONFIG_PATH, CONFIG_TEMP_PATH, CONFIG_BACKUP_PATH,
      exists, remove, rename);
  if (!result.committed) {
    Serial.printf("MQTT settings: commit failed (%u); retaining verified temp\n",
                  (unsigned int)result.failure);
    return false;
  }

  _loaded_source = mqtt_prefs::RecoverySource::Primary;
  return true;
}

bool MqttSettingsStore::promoteRecoveredTemp() {
  if (_fs == nullptr) return false;

  const auto exists = [this](const char *path) { return _fs->exists(path); };
  const auto remove = [this](const char *path) { return _fs->remove(path); };
  const auto rename = [this](const char *from, const char *to) {
    return _fs->rename(from, to);
  };
  const mqtt_prefs::RecoveryCommitResult result = mqtt_prefs::commitVerifiedTemp(
      mqtt_prefs::RecoverySource::Temp, CONFIG_PATH, CONFIG_TEMP_PATH, CONFIG_BACKUP_PATH,
      exists, remove, rename);
  if (!result.committed) {
    Serial.printf("MQTT settings: recovery temp promotion failed (%u); temp retained\n",
                  (unsigned int)result.failure);
    return false;
  }
  return true;
}

uint32_t MqttSettingsStore::configCrc32() const {
  // Same bytes in the same order as the old whole-image CRC: header, shared,
  // then each broker — just composed piecewise so nothing multi-KB is ever
  // placed on the stack.
  struct V4Header { uint32_t magic; uint16_t version; uint8_t broker_count; uint8_t reserved; };
  V4Header header;
  header.magic = CONFIG_MAGIC;
  header.version = CONFIG_VERSION;
  header.broker_count = (uint8_t)brokerCount();
  header.reserved = 0;

  uint32_t crc = 0xFFFFFFFFUL;
  mqtt_prefs::crc32Update(crc, reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  MqttSharedConfig shared = _shared;
  sanitizeShared(shared);
  mqtt_prefs::crc32Update(crc, reinterpret_cast<const uint8_t *>(&shared), sizeof(shared));
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    MqttBrokerConfig broker = _brokers[i];
    sanitizeBroker(broker);
    mqtt_prefs::crc32Update(crc, reinterpret_cast<const uint8_t *>(&broker), sizeof(broker));
  }
  return mqtt_prefs::crc32Finish(crc);
}

void MqttSettingsStore::resetToDefaults() {
  // Note: does not clear the write-hold - only an explicit `mqtt reset`
  // (clearWriteHold()) drops it, so a downgrade cannot clobber a newer file.
  memset(&_shared, 0, sizeof(_shared));
  memset(_brokers, 0, sizeof(_brokers));

  StrHelper::strncpy(_shared.wifi_ssid, WIFI_SSID, sizeof(_shared.wifi_ssid));
  StrHelper::strncpy(_shared.wifi_pwd, WIFI_PWD, sizeof(_shared.wifi_pwd));
  StrHelper::strncpy(_shared.model, MQTT_MODEL, sizeof(_shared.model));
  StrHelper::strncpy(_shared.client_version, MQTT_CLIENT_VERSION, sizeof(_shared.client_version));

  StrHelper::strncpy(_brokers[0].uri, MQTT_BROKER1_URI, sizeof(_brokers[0].uri));
  StrHelper::strncpy(_brokers[0].username, MQTT_BROKER1_USERNAME, sizeof(_brokers[0].username));
  StrHelper::strncpy(_brokers[0].password, MQTT_BROKER1_PASSWORD, sizeof(_brokers[0].password));
  StrHelper::strncpy(_brokers[0].topic_root, MQTT_BROKER1_TOPIC_ROOT, sizeof(_brokers[0].topic_root));
  StrHelper::strncpy(_brokers[0].iata, MQTT_BROKER1_IATA, sizeof(_brokers[0].iata));
  _brokers[0].retain_status = MQTT_BROKER1_RETAIN_STATUS ? 1 : 0;
  _brokers[0].enabled = MQTT_BROKER1_ENABLED ? 1 : 0;
  _brokers[0].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  StrHelper::strncpy(_brokers[1].uri, MQTT_BROKER2_URI, sizeof(_brokers[1].uri));
  StrHelper::strncpy(_brokers[1].username, MQTT_BROKER2_USERNAME, sizeof(_brokers[1].username));
  StrHelper::strncpy(_brokers[1].password, MQTT_BROKER2_PASSWORD, sizeof(_brokers[1].password));
  StrHelper::strncpy(_brokers[1].topic_root, MQTT_BROKER2_TOPIC_ROOT, sizeof(_brokers[1].topic_root));
  StrHelper::strncpy(_brokers[1].iata, MQTT_BROKER2_IATA, sizeof(_brokers[1].iata));
  _brokers[1].retain_status = MQTT_BROKER2_RETAIN_STATUS ? 1 : 0;
  _brokers[1].enabled = MQTT_BROKER2_ENABLED ? 1 : 0;
  _brokers[1].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  StrHelper::strncpy(_brokers[2].uri, MQTT_BROKER3_URI, sizeof(_brokers[2].uri));
  StrHelper::strncpy(_brokers[2].username, MQTT_BROKER3_USERNAME, sizeof(_brokers[2].username));
  StrHelper::strncpy(_brokers[2].password, MQTT_BROKER3_PASSWORD, sizeof(_brokers[2].password));
  StrHelper::strncpy(_brokers[2].topic_root, MQTT_BROKER3_TOPIC_ROOT, sizeof(_brokers[2].topic_root));
  StrHelper::strncpy(_brokers[2].iata, MQTT_BROKER3_IATA, sizeof(_brokers[2].iata));
  _brokers[2].retain_status = MQTT_BROKER3_RETAIN_STATUS ? 1 : 0;
  _brokers[2].enabled = MQTT_BROKER3_ENABLED ? 1 : 0;
  _brokers[2].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  StrHelper::strncpy(_brokers[3].uri, MQTT_BROKER4_URI, sizeof(_brokers[3].uri));
  StrHelper::strncpy(_brokers[3].username, MQTT_BROKER4_USERNAME, sizeof(_brokers[3].username));
  StrHelper::strncpy(_brokers[3].password, MQTT_BROKER4_PASSWORD, sizeof(_brokers[3].password));
  StrHelper::strncpy(_brokers[3].topic_root, MQTT_BROKER4_TOPIC_ROOT, sizeof(_brokers[3].topic_root));
  StrHelper::strncpy(_brokers[3].iata, MQTT_BROKER4_IATA, sizeof(_brokers[3].iata));
  _brokers[3].retain_status = MQTT_BROKER4_RETAIN_STATUS ? 1 : 0;
  _brokers[3].enabled = MQTT_BROKER4_ENABLED ? 1 : 0;
  _brokers[3].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  StrHelper::strncpy(_brokers[4].uri, MQTT_BROKER5_URI, sizeof(_brokers[4].uri));
  StrHelper::strncpy(_brokers[4].username, MQTT_BROKER5_USERNAME, sizeof(_brokers[4].username));
  StrHelper::strncpy(_brokers[4].password, MQTT_BROKER5_PASSWORD, sizeof(_brokers[4].password));
  StrHelper::strncpy(_brokers[4].topic_root, MQTT_BROKER5_TOPIC_ROOT, sizeof(_brokers[4].topic_root));
  StrHelper::strncpy(_brokers[4].iata, MQTT_BROKER5_IATA, sizeof(_brokers[4].iata));
  _brokers[4].retain_status = MQTT_BROKER5_RETAIN_STATUS ? 1 : 0;
  _brokers[4].enabled = MQTT_BROKER5_ENABLED ? 1 : 0;
  _brokers[4].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  StrHelper::strncpy(_brokers[5].uri, MQTT_BROKER6_URI, sizeof(_brokers[5].uri));
  StrHelper::strncpy(_brokers[5].username, MQTT_BROKER6_USERNAME, sizeof(_brokers[5].username));
  StrHelper::strncpy(_brokers[5].password, MQTT_BROKER6_PASSWORD, sizeof(_brokers[5].password));
  StrHelper::strncpy(_brokers[5].topic_root, MQTT_BROKER6_TOPIC_ROOT, sizeof(_brokers[5].topic_root));
  StrHelper::strncpy(_brokers[5].iata, MQTT_BROKER6_IATA, sizeof(_brokers[5].iata));
  _brokers[5].retain_status = MQTT_BROKER6_RETAIN_STATUS ? 1 : 0;
  _brokers[5].enabled = MQTT_BROKER6_ENABLED ? 1 : 0;
  _brokers[5].neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    sanitizeBroker(_brokers[i]);
  }
}


void MqttSettingsStore::clearWriteHold() {
  if (_prefs_write_hold) {
    _prefs_write_hold = false;
    Serial.println("MQTT settings: write hold cleared");
  }
}

int MqttSettingsStore::brokerCount() const {
  int count = 0;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_brokers[i].enabled) count++;
  }
  return count;
}

bool MqttSettingsStore::brokerCredentialsAllowed(int idx) const {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  const MqttBrokerConfig &cfg = _brokers[idx];
  if (cfg.username[0] == '\0' && cfg.password[0] == '\0') return true;
  return strncmp(cfg.uri, "mqtt://", 7) != 0 && strncmp(cfg.uri, "ws://", 5) != 0;
}

int MqttSettingsStore::parseKey(const char *key, const char **key_out) {
  if (strncmp(key, "mqtt.", 5) == 0) {
    key += 5;
  }

  // Check for "N." prefix where N is 1-6
  if (key[0] >= '1' && key[0] <= '0' + MQTT_MAX_BROKERS && key[1] == '.') {
    *key_out = key + 2;
    return key[0] - '1'; // 0-based index
  }
  *key_out = key;

  // Shared keys return -1
  if (strcmp(key, "wifi.ssid") == 0 || strcmp(key, "wifi.pass") == 0 ||
      strcmp(key, "model") == 0 || strcmp(key, "client.version") == 0 ||
      strcmp(key, "boot_count") == 0) {
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
  char numeric[16];

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
    } else if (strcmp(field, "boot_count") == 0) {
      snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)_boot_count);
      value = numeric;
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
  } else if (strcmp(field, "neighbor.interval") == 0) {
    snprintf(numeric, sizeof(numeric), "%lu", (unsigned long)b.neighbor_interval_secs);
    value = numeric;
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
    } else if (strcmp(field, "boot_count") == 0) {
      char *end = nullptr;
      unsigned long parsed = strtoul(value, &end, 10);
      if (end == value || *end != '\0') return false;
      _boot_count = (uint32_t)parsed;
    } else {
      return false;
    }
    sanitizeShared(_shared);
    return true;
  }

  // Per-broker keys
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  MqttBrokerConfig &b = _brokers[idx];
  MqttBrokerConfig previous = b;

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
  } else if (strcmp(field, "neighbor.interval") == 0) {
    char *end = nullptr;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') return false;
    b.neighbor_interval_secs = (uint32_t)parsed;
  } else {
    return false;
  }

  if (!validBrokerTopicConfig(b)) {
    b = previous;
    return false;
  }
  sanitizeBroker(b);
  if (!brokerCredentialsAllowed(idx)) {
    b = previous;
    return false;
  }
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
  if (cfg.neighbor_interval_secs == 0) {
    cfg.neighbor_interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;
  }
  if (cfg.neighbor_interval_secs < MQTT_NEIGHBOR_MIN_INTERVAL_SECS) {
    cfg.neighbor_interval_secs = MQTT_NEIGHBOR_MIN_INTERVAL_SECS;
  }

  if (!validBrokerTopicConfig(cfg)) {
    StrHelper::strncpy(cfg.topic_root, MQTT_TOPIC_ROOT, sizeof(cfg.topic_root));
    StrHelper::strncpy(cfg.iata, MQTT_IATA, sizeof(cfg.iata));
  }
}

#endif
