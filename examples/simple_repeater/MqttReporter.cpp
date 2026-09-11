#include "MqttReporter.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include "MyMesh.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <SPIFFS.h>
#include <math.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(__has_include)
#if __has_include(<driver/temperature_sensor.h>)
#include <driver/temperature_sensor.h>
#define MESHCORE_HAS_TEMPERATURE_SENSOR_DRIVER 1
#endif
#endif

extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

#ifndef MESHCORE_GIT_COMMIT
  #define MESHCORE_GIT_COMMIT "unknown"
#endif

std::atomic<MqttReporter *> MqttReporter::s_instance{nullptr};

namespace {

constexpr unsigned long MQTT_DEBUG_STATS_INTERVAL_MS = 60000UL;
constexpr unsigned long MQTT_NTP_RETRY_INTERVAL_MS = 30000UL;
constexpr unsigned long MQTT_NTP_TIMEOUT_MS = 10000UL;
constexpr uint32_t MQTT_CONNECT_RETRY_BASE_MS = 5000UL;
constexpr uint32_t MQTT_CONNECT_RETRY_MAX_MS = 300000UL;
constexpr time_t MQTT_VALID_EPOCH = 1700000000;

const char *resetReasonString(int reason) {
  // Use numeric values so this remains buildable with older ESP-IDF headers
  // whose esp_reset_reason_t enum predates USB/PWR_GLITCH additions.
  switch (reason) {
    case 1: return "POWERON";
    case 2: return "EXT";
    case 3: return "SW";
    case 4: return "PANIC";
    case 5: return "INT_WDT";
    case 6: return "TASK_WDT";
    case 7: return "WDT";
    case 8: return "DEEPSLEEP";
    case 9: return "BROWNOUT";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 13: return "EFUSE";
    case 14: return "PWR_GLITCH";
    case 15: return "CPU_LOCKUP";
    default: return "UNKNOWN";
  }
}

String expandTopicTokens(const char *topic_root, const char *iata, const char *origin_id);

String sanitizeBrokerUri(const char *uri) {
  String sanitized = uri ? String(uri) : String("");
  int scheme_end = sanitized.indexOf("://");
  if (scheme_end >= 0) {
    int authority_start = scheme_end + 3;
    int authority_end = sanitized.indexOf('/', authority_start);
    if (authority_end < 0) authority_end = sanitized.length();
    int credentials_end = sanitized.indexOf('@', authority_start);
    if (credentials_end >= authority_start && credentials_end < authority_end) {
      sanitized.remove(authority_start, credentials_end - authority_start + 1);
    }
  }
  return sanitized;
}

String buildNeighborsTopicPath(const char *topic_root, const char *iata, const char *origin_id) {
  String topic = expandTopicTokens(topic_root, iata, origin_id);
  if (topic.endsWith("/packets")) {
    topic.remove(topic.length() - 8);
    topic += "/neighbors";
    return topic;
  }
  if (topic.endsWith("/status")) {
    topic.remove(topic.length() - 7);
    topic += "/neighbors";
    return topic;
  }
  if (!topic.endsWith("/neighbors")) topic += "/neighbors";
  return topic;
}

float readBoardTemperatureC() {
#if defined(MESHCORE_HAS_TEMPERATURE_SENSOR_DRIVER)
  temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  temperature_sensor_handle_t sensor = nullptr;
  if (temperature_sensor_install(&config, &sensor) != ESP_OK || sensor == nullptr) {
    return NAN;
  }
  float temperature_c = NAN;
  esp_err_t result = temperature_sensor_enable(sensor);
  if (result == ESP_OK) {
    result = temperature_sensor_get_celsius(sensor, &temperature_c);
    temperature_sensor_disable(sensor);
  }
  temperature_sensor_uninstall(sensor);
  return result == ESP_OK && isfinite(temperature_c) ? temperature_c : NAN;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  // Arduino's IDF 4.4 compatibility layer exposes the same S3 internal
  // sensor through temperatureRead(); use it when the newer driver header is
  // not part of the active framework include set.
  float temperature_c = temperatureRead();
  return isfinite(temperature_c) ? temperature_c : NAN;
#else
  return NAN;
#endif
}

#ifndef MQTT_CHANNEL_KEY_ID
  #define MQTT_CHANNEL_KEY_ID ""
#endif

String channelKeyId() {
  return String(MQTT_CHANNEL_KEY_ID);
}

String replaceToken(String value, const char *token, const char *replacement) {
  value.replace(token, replacement);
  return value;
}

String expandTopicTokens(const char *topic_root, const char *iata, const char *origin_id) {
  String topic = topic_root ? String(topic_root) : String("");
  topic = replaceToken(topic, "{IATA}", iata);
  topic = replaceToken(topic, "<IATA>", iata);
  topic = replaceToken(topic, "{PUBLIC_KEY}", origin_id);
  topic = replaceToken(topic, "<PUBLIC_KEY>", origin_id);
  return topic;
}

String buildStatusTopicPath(const char *topic_root, const char *iata, const char *origin_id) {
  String topic = expandTopicTokens(topic_root, iata, origin_id);
  if (topic.endsWith("/packets")) {
    topic.remove(topic.length() - 8);
    topic += "/status";
    return topic;
  }
  if (topic.endsWith("/status")) {
    return topic;
  }
  return topic;
}

String buildPacketsTopicPath(const char *topic_root, const char *iata, const char *origin_id) {
  String topic = expandTopicTokens(topic_root, iata, origin_id);
  if (topic.endsWith("/status")) {
    topic.remove(topic.length() - 7);
    topic += "/packets";
    return topic;
  }
  if (topic.endsWith("/packets")) {
    return topic;
  }
  return topic;
}

} // namespace

MqttReporter::MqttReporter(MyMesh &mesh, mesh::RTCClock &clock)
    : _mesh(&mesh), _clock(&clock) {
  _last_wifi_attempt = 0;
  _wifi_attempted = false;
  _last_stats_print = 0;
  _last_ntp_attempt = 0;
  _ntp_attempted = false;
  _ntp_sync_started_at = 0;
  _ntp_sync_pending = false;
  _time_synced = false;
  _ntp_synced_at_ms = 0;
  _origin_id[0] = '\0';
  _client_id[0] = '\0';
  StrHelper::strncpy(_reset_reason, "UNKNOWN", sizeof(_reset_reason));
  _boot_count = 0;
  _config_crc32 = 0;
  _max_loop_ms = 0;
  _max_loop_at_ms = 0;
  _last_rx_rssi = 0;
  _last_rx_snr = 0.0f;
  _last_tx_fail_reason = 0;
  _rx_publish_calls = 0;
  _tx_publish_calls = 0;
  _tx_fail_publish_calls = 0;
  _publish_skipped_no_connection = 0;
  _wifi_reconnect_attempts = 0;
  _wifi_consecutive_failures = 0;
  _loop_iterations = 0;
  _min_free_heap = UINT32_MAX;
  _last_wifi_disconnect_reason = -1;
  _wifi_got_ip = false;
  _wifi_connected_since_ms = 0;
  _wifi_last_ip = "";
  _identity_strings_dirty = true;
  _callback_queue = nullptr;
  _last_cpu_sample_ms = 0;
  _idle_pct_core0 = -1.0f;
  _idle_pct_core1 = -1.0f;
  _last_idle_tick_count[0] = 0;
  _last_idle_tick_count[1] = 0;
  _last_total_runtime = 0;
  memset(_heard_nodes, 0, sizeof(_heard_nodes));
  memset(_neighbors, 0, sizeof(_neighbors));

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    _clients[i].client = nullptr;
    _clients[i].started = false;
    _clients[i].connected_since_ms = 0;
    _clients[i].connected = false;
    _clients[i].next_connect_attempt_ms = 0;
    _clients[i].status_topic[0] = '\0';
    _clients[i].packets_topic[0] = '\0';
    _clients[i].neighbors_topic[0] = '\0';
    _clients[i].last_status_publish = 0;
    _clients[i].last_neighbors_publish = 0;
    _clients[i].queue_head = 0;
    _clients[i].queue_tail = 0;
    _clients[i].queue_count = 0;
    _clients[i].status_queued = false;
    _clients[i].online_status_pending = false;
    _clients[i].connect_attempts = 0;
    _clients[i].connect_start_failures = 0;
    _clients[i].connect_events = 0;
    _clients[i].disconnect_events = 0;
    _clients[i].error_events = 0;
    _clients[i].status_publish_count = 0;
    _clients[i].packet_publish_count = 0;
    _clients[i].session_status_publish_count = 0;
    _clients[i].session_packet_publish_count = 0;
    _clients[i].publish_failures = 0;
    _clients[i].queue_drops = 0;
    memset(_clients[i].reconnect_attempt_ms, 0, sizeof(_clients[i].reconnect_attempt_ms));
    _clients[i].reconnect_attempt_head = 0;
    _clients[i].reconnect_attempt_count = 0;
    _clients[i].last_offline_epoch = 0;
    for (uint8_t q = 0; q < MQTT_PUBLISH_QUEUE_SIZE; q++) {
      _clients[i].publish_queue[q].qos = 0;
      _clients[i].publish_queue[q].retain = false;
      _clients[i].publish_queue[q].is_status = false;
      _clients[i].publish_queue[q].pending = false;
    }
    _event_ctx[i].reporter = this;
    _event_ctx[i].broker_idx = i;
  }

  // WiFi callbacks are registered in begin(), after construction is complete.
  s_instance.store(this, std::memory_order_release);
}

MqttReporter::~MqttReporter() {
  MqttReporter *expected = this;
  s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].client != nullptr) {
      esp_mqtt_client_stop(_clients[i].client);
      esp_mqtt_client_destroy(_clients[i].client);
      _clients[i].client = nullptr;
    }
  }
  if (_callback_queue != nullptr) {
    vQueueDelete(_callback_queue);
    _callback_queue = nullptr;
  }
}

void MqttReporter::begin(FILESYSTEM *fs) {
  _settings.begin(fs);
  _config_crc32 = _settings.configCrc32();
  _boot_count = _settings.incrementBootCount();
  StrHelper::strncpy(_reset_reason, resetReasonString((int)esp_reset_reason()), sizeof(_reset_reason));
  ensureIdentityStrings();
  _callback_queue = xQueueCreate(MQTT_CALLBACK_QUEUE_DEPTH, sizeof(CallbackEvent));
  if (_callback_queue == nullptr) {
    Serial.println("MQTT reporter: callback queue allocation failed");
  }
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_INFO);
  esp_log_level_set("TRANSPORT_BASE", ESP_LOG_INFO);
  esp_log_level_set("TRANSPORT_WS", ESP_LOG_INFO);
  esp_log_level_set("TRANS_SSL", ESP_LOG_INFO);
  esp_log_level_set("esp-tls", ESP_LOG_INFO);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    MqttReporter *self = s_instance.load(std::memory_order_acquire);
    if (self == nullptr) return;
    CallbackEvent callback = {};
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      callback.type = CallbackEventType::WifiDisconnected;
      callback.wifi_reason = info.wifi_sta_disconnected.reason;
      self->enqueueCallbackEvent(callback);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      callback.type = CallbackEventType::WifiGotIp;
      self->enqueueCallbackEvent(callback);
    }
  });

  const MqttSharedConfig &shared = _settings.shared();
  Serial.printf("MQTT reporter: WiFi SSID='%s'\n", shared.wifi_ssid);
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (b.enabled && b.uri[0] != '\0') {
      String safe_uri = sanitizeBrokerUri(b.uri);
      Serial.printf("MQTT reporter: broker %d URI='%s'\n", i + 1, safe_uri.c_str());
    }
  }
  connectWiFi();
}

void MqttReporter::loop() {
  const int64_t loop_started_us = esp_timer_get_time();
  _loop_iterations++;
  uint32_t free_heap = ESP.getFreeHeap();
  if (free_heap < _min_free_heap) _min_free_heap = free_heap;
  unsigned long now = millis();
  processCallbackEvents();
  if (_identity_strings_dirty) ensureIdentityStrings();

  if (WiFi.status() != WL_CONNECTED) {
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      _clients[i].connected = false;
      _clients[i].online_status_pending = false;
      clearPublishQueue(i);
    }
    connectWiFi();
    maybePrintPeriodicStats();
    finishLoop(loop_started_us);
    return;
  }
  _wifi_consecutive_failures = 0;

  // MQTT payloads carry timestamps on every transport, not only TLS brokers.
  bool mqtt_reporting_enabled = false;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &broker = _settings.broker(i);
    if (broker.enabled && broker.uri[0] != '\0') {
      mqtt_reporting_enabled = true;
      break;
    }
  }

  if (!_time_synced && mqtt_reporting_enabled) {
    checkNtpSyncComplete();
    if (!_time_synced && !_ntp_sync_pending &&
        (!_ntp_attempted || (uint32_t)(now - _last_ntp_attempt) >= MQTT_NTP_RETRY_INTERVAL_MS)) {
      _last_ntp_attempt = now;
      _ntp_attempted = true;
      syncTimeFromNtp();
    }
  }

  // Connect/maintain all enabled brokers
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (!b.enabled || b.uri[0] == '\0') continue;
    if (!_clients[i].started) {
      connectMQTT(i);
    }
  }

  // Periodic status for connected brokers
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    BrokerClient &bc = _clients[i];
    if (bc.connected) {
      if (bc.online_status_pending) {
        bc.online_status_pending = false;
        publishStatus(i, "online");
      }
      if (now - _clients[i].last_status_publish >= (unsigned long)MQTT_STATUS_INTERVAL_SECS * 1000UL) {
        publishStatus(i, "online");
      }
    }
  }

  maybePublishNeighbors(now);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected) {
      drainPublishQueue(i);
    } else {
      clearPublishQueue(i);
    }
  }

  maybePrintPeriodicStats();
  finishLoop(loop_started_us);
}

void MqttReporter::finishLoop(int64_t started_us) {
  int64_t elapsed_us = esp_timer_get_time() - started_us;
  if (elapsed_us < 0) elapsed_us = 0;
  uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
  if (elapsed_ms > _max_loop_ms) {
    _max_loop_ms = elapsed_ms;
    _max_loop_at_ms = millis();
  }
}

void MqttReporter::publishRxRaw(const uint8_t raw[], int len) {
  if (raw == nullptr || len <= 0) {
    _last_rx_raw = "";
    return;
  }
  _last_rx_raw = bytesToHex(raw, len);
}

void MqttReporter::clearPendingRxRaw() {
  _last_rx_raw = "";
}

void MqttReporter::publishRxPacket(mesh::Packet *pkt, int len, float score, int rssi, float snr, uint32_t duration_ms) {
  if (pkt == nullptr) return;
  _rx_publish_calls++;
  _last_rx_rssi = rssi;
  _last_rx_snr = snr;

  const uint32_t now_ms = millis();
  uint8_t source_id[PUB_KEY_SIZE];
  if (_mesh->resolvePacketSourceId(pkt, source_id)) {
    upsertHeardNode(source_id, now_ms);
    upsertNeighbor(source_id, rssi, snr, now_ms);
  }

  String raw_hex = _last_rx_raw;
  _last_rx_raw = "";

  if (!anyBrokerConnected()) {
    _publish_skipped_no_connection++;
    return;
  }

  if (raw_hex.length() == 0) {
    raw_hex = bytesToHex(pkt->payload, pkt->payload_len);
  }
  String payload = buildPacketPayload("rx", pkt, len, raw_hex, score, rssi, snr, duration_ms, true);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, payload, 0, false, false);
    }
  }
}

void MqttReporter::publishTxPacket(mesh::Packet *pkt, int len) {
  if (pkt == nullptr) return;
  _tx_publish_calls++;

  if (!anyBrokerConnected()) {
    _publish_skipped_no_connection++;
    return;
  }

  uint8_t raw[MAX_TRANS_UNIT];
  int raw_len = pkt->writeTo(raw);
  String payload = buildPacketPayload("tx", pkt, len, bytesToHex(raw, raw_len), 0.0f, 0, 0.0f, 0, false);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, payload, 0, false, false);
    }
  }
}

void MqttReporter::publishTxFail(mesh::Packet *pkt, int len, int reason) {
  if (pkt == nullptr) return;
  _tx_fail_publish_calls++;
  _last_tx_fail_reason = reason;

  if (!anyBrokerConnected()) {
    _publish_skipped_no_connection++;
    return;
  }

  uint8_t raw[MAX_TRANS_UNIT];
  int raw_len = pkt->writeTo(raw);
  String payload = buildPacketPayload("tx_fail", pkt, len, bytesToHex(raw, raw_len), 0.0f, 0, 0.0f, 0, false);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, payload, 0, false, false);
    }
  }
}

void MqttReporter::ensureIdentityStrings() {
  if (_origin_id[0] == '\0') {
    const mesh::LocalIdentity &self = _mesh->getSelfId();
    static const char hex_chars[] = "0123456789ABCDEF";
    for (size_t i = 0; i < PUB_KEY_SIZE; ++i) {
      _origin_id[i * 2] = hex_chars[self.pub_key[i] >> 4];
      _origin_id[i * 2 + 1] = hex_chars[self.pub_key[i] & 0x0F];
    }
    _origin_id[PUB_KEY_SIZE * 2] = '\0';

    snprintf(_client_id, sizeof(_client_id), "meshcore_%.*s", 16, _origin_id);
  }

  if (!_identity_strings_dirty) return;

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (!b.enabled) {
      _clients[i].status_topic[0] = '\0';
      _clients[i].packets_topic[0] = '\0';
      _clients[i].neighbors_topic[0] = '\0';
      _clients[i].offline_payload = "";
      continue;
    }
    String status_topic = buildStatusTopicPath(b.topic_root, b.iata, _origin_id);
    String packets_topic = buildPacketsTopicPath(b.topic_root, b.iata, _origin_id);
    String neighbors_topic = buildNeighborsTopicPath(b.topic_root, b.iata, _origin_id);
    StrHelper::strncpy(_clients[i].status_topic, status_topic.c_str(), sizeof(_clients[i].status_topic));
    StrHelper::strncpy(_clients[i].packets_topic, packets_topic.c_str(), sizeof(_clients[i].packets_topic));
    StrHelper::strncpy(_clients[i].neighbors_topic, neighbors_topic.c_str(), sizeof(_clients[i].neighbors_topic));
    _clients[i].offline_payload = buildStatusPayload(i, "offline");
  }
  _identity_strings_dirty = false;
}

void MqttReporter::resetBrokerConnection(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (bc.client != nullptr) {
    esp_mqtt_client_stop(bc.client);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
  }
  clearPublishQueue(idx);
  bc.started = false;
  bc.connected = false;
  bc.online_status_pending = false;
  bc.next_connect_attempt_ms = 0;
}

void MqttReporter::resetAllConnections() {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    resetBrokerConnection(i);
  }
  _last_wifi_attempt = 0;
  _wifi_attempted = false;
  _wifi_consecutive_failures = 0;

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.disconnect(true, false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
}

bool MqttReporter::connectWiFi() {
  const MqttSharedConfig &shared = _settings.shared();
  if (WiFi.status() == WL_CONNECTED) return true;
  if (shared.wifi_ssid[0] == '\0') return false;

  unsigned long now = millis();
  uint8_t shift = _wifi_consecutive_failures > 0 ? _wifi_consecutive_failures - 1 : 0;
  if (shift > 6) shift = 6;
  unsigned long retry_delay = 5000UL << shift;
  if (retry_delay > 300000UL) retry_delay = 300000UL;
  if (_wifi_attempted && (uint32_t)(now - _last_wifi_attempt) < retry_delay) return false;
  _last_wifi_attempt = now;
  _wifi_attempted = true;
  _wifi_reconnect_attempts++;
  if (_wifi_consecutive_failures < UINT8_MAX) _wifi_consecutive_failures++;

  Serial.printf("MQTT reporter: connecting WiFi to '%s' (retry in %lu ms if needed)\n",
                shared.wifi_ssid, retry_delay);
  WiFi.begin(shared.wifi_ssid, shared.wifi_pwd);
  return false;
}

bool MqttReporter::connectMQTT(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  BrokerClient &bc = _clients[idx];
  const MqttBrokerConfig &broker = _settings.broker(idx);

  if (bc.started) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (broker.uri[0] == '\0') return false;
  if (brokerNeedsTimeSync(idx) && !_time_synced) return false;
  if (!_settings.brokerCredentialsAllowed(idx)) {
    Serial.printf("MQTT reporter: broker %d refusing cleartext URI while credentials are configured\n", idx + 1);
    bc.next_connect_attempt_ms = millis() + MQTT_CONNECT_RETRY_MAX_MS;
    return false;
  }
  uint32_t now = millis();
  if ((int32_t)(now - bc.next_connect_attempt_ms) < 0) return false;
  bc.connect_attempts++;
  recordReconnectAttempt(bc, now);

  // A disconnect/error event leaves the old client handle allocated. Dispose of
  // it here, outside the MQTT event callback, before creating its replacement.
  if (bc.client != nullptr) {
    esp_mqtt_client_stop(bc.client);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
  }
  clearPublishQueue(idx);
  bc.connected = false;
  bc.online_status_pending = false;

  // Use a unique client_id per broker
  char broker_client_id[48];
  snprintf(broker_client_id, sizeof(broker_client_id), "%s_%d", _client_id, idx + 1);

  String safe_uri = sanitizeBrokerUri(broker.uri);
  Serial.printf(
      "MQTT reporter: init broker %d heap=%u uri='%s' user='%s'\n",
      idx + 1,
      (unsigned int)ESP.getFreeHeap(),
      safe_uri.c_str(),
      broker.username);

  esp_mqtt_client_config_t mqtt_config = {};
  mqtt_config.uri = broker.uri;
  mqtt_config.client_id = broker_client_id;
  mqtt_config.username = broker.username;
  mqtt_config.password = broker.password;
  mqtt_config.keepalive = 60;
  mqtt_config.disable_auto_reconnect = false;
  // rev3 status payloads (full telemetry + nested mqtt block) are ~1.9 KB and
  // are also used as the LWT, so the CONNECT packet alone can exceed the
  // default 2048-byte buffers. Without this headroom esp-mqtt fails every
  // connect with "Connect message cannot be created".
  mqtt_config.buffer_size = 4096;
  mqtt_config.out_buffer_size = 4096;
  mqtt_config.lwt_topic = bc.status_topic;
  mqtt_config.lwt_msg = bc.offline_payload.c_str();
  mqtt_config.lwt_qos = 0;
  mqtt_config.lwt_retain = broker.retain_status != 0;
  mqtt_config.user_context = &_event_ctx[idx];
  mqtt_config.event_handle = mqttEventHandler;
  if (strncmp(broker.uri, "wss://", 6) == 0 || strncmp(broker.uri, "mqtts://", 8) == 0) {
    mqtt_config.crt_bundle_attach = esp_crt_bundle_attach;
  }

  bc.client = esp_mqtt_client_init(&mqtt_config);
  if (bc.client == nullptr) {
    bc.connect_start_failures++;
    uint8_t shift = bc.connect_start_failures > 1 ? (uint8_t)(bc.connect_start_failures - 1) : 0;
    if (shift > 6) shift = 6;
    uint32_t retry_delay = MQTT_CONNECT_RETRY_BASE_MS << shift;
    if (retry_delay > MQTT_CONNECT_RETRY_MAX_MS) retry_delay = MQTT_CONNECT_RETRY_MAX_MS;
    bc.next_connect_attempt_ms = millis() + retry_delay;
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_init failed\n", idx + 1);
    return false;
  }

  // Set before start so a fast disconnect/error callback cannot be overwritten
  // by a late assignment after esp_mqtt_client_start() returns.
  bc.started = true;
  if (esp_mqtt_client_start(bc.client) != ESP_OK) {
    bc.connect_start_failures++;
    uint8_t shift = bc.connect_start_failures > 1 ? (uint8_t)(bc.connect_start_failures - 1) : 0;
    if (shift > 6) shift = 6;
    uint32_t retry_delay = MQTT_CONNECT_RETRY_BASE_MS << shift;
    if (retry_delay > MQTT_CONNECT_RETRY_MAX_MS) retry_delay = MQTT_CONNECT_RETRY_MAX_MS;
    bc.next_connect_attempt_ms = millis() + retry_delay;
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_start failed\n", idx + 1);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
    bc.started = false;
    return false;
  }

  Serial.printf("MQTT reporter: broker %d MQTT client started\n", idx + 1);
  return true;
}

void MqttReporter::syncTimeFromNtp() {
  configTime(0, 0, MQTT_NTP_SERVER);
  _ntp_sync_started_at = millis();
  _ntp_sync_pending = true;
  Serial.println("MQTT reporter: NTP sync started");
}

void MqttReporter::checkNtpSyncComplete() {
  time_t now = time(nullptr);
  if (now >= MQTT_VALID_EPOCH) {
    _clock->setCurrentTime((uint32_t)now);
    _time_synced = true;
    _ntp_synced_at_ms = millis();
    _ntp_sync_pending = false;
    Serial.println("MQTT reporter: NTP sync complete");
    return;
  }

  if (_ntp_sync_pending &&
      (uint32_t)(millis() - _ntp_sync_started_at) >= MQTT_NTP_TIMEOUT_MS) {
    _ntp_sync_pending = false;
    Serial.println("MQTT reporter: NTP sync timed out; will retry");
  }
}

void MqttReporter::publishStatus(int idx, const char *status) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) return;
  if (bc.status_queued) return;

  String payload = buildStatusPayload(idx, status);
  enqueuePublish(idx, bc.status_topic, payload, 0,
                 _settings.broker(idx).retain_status != 0, true);
}

bool MqttReporter::enqueuePublish(
    int idx, const char *topic, const String &payload, uint8_t qos, bool retain, bool is_status) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS || topic == nullptr) return false;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) return false;
  if (is_status && bc.status_queued) return true;

  if (bc.queue_count == MQTT_PUBLISH_QUEUE_SIZE) {
    PublishEntry &dropped = bc.publish_queue[bc.queue_head];
    if (dropped.is_status) bc.status_queued = false;
    dropped.topic = "";
    dropped.payload = "";
    dropped.pending = false;
    bc.queue_head = (uint8_t)((bc.queue_head + 1) % MQTT_PUBLISH_QUEUE_SIZE);
    bc.queue_count--;
    bc.queue_drops++;
    bc.publish_failures++;
  }

  PublishEntry &entry = bc.publish_queue[bc.queue_tail];
  entry.topic = topic;
  entry.payload = payload;
  if (entry.topic.length() != strlen(topic) || entry.payload.length() != payload.length()) {
    entry.topic = "";
    entry.payload = "";
    entry.pending = false;
    bc.publish_failures++;
    return false;
  }
  entry.qos = qos;
  entry.retain = retain;
  entry.is_status = is_status;
  entry.pending = true;
  bc.queue_tail = (uint8_t)((bc.queue_tail + 1) % MQTT_PUBLISH_QUEUE_SIZE);
  bc.queue_count++;
  if (is_status) bc.status_queued = true;
  return true;
}

void MqttReporter::drainPublishQueue(int idx, uint8_t max_publishes) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) {
    clearPublishQueue(idx);
    return;
  }

  uint8_t published = 0;
  while (bc.queue_count > 0 && published < max_publishes) {
    PublishEntry &entry = bc.publish_queue[bc.queue_head];
    bool was_status = entry.is_status;
    int message_id = -1;
    if (entry.pending) {
      message_id = esp_mqtt_client_publish(
          bc.client,
          entry.topic.c_str(),
          entry.payload.c_str(),
          entry.payload.length(),
          entry.qos,
          entry.retain);
    }

    if (message_id < 0) {
      bc.publish_failures++;
    } else if (was_status) {
      bc.status_publish_count++;
      bc.session_status_publish_count++;
      bc.last_status_publish = millis();
    } else {
      bc.packet_publish_count++;
      bc.session_packet_publish_count++;
    }

    if (was_status) bc.status_queued = false;
    entry.topic = "";
    entry.payload = "";
    entry.pending = false;
    entry.is_status = false;
    bc.queue_head = (uint8_t)((bc.queue_head + 1) % MQTT_PUBLISH_QUEUE_SIZE);
    bc.queue_count--;
    published++;
  }
}

void MqttReporter::clearPublishQueue(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  while (bc.queue_count > 0) {
    PublishEntry &entry = bc.publish_queue[bc.queue_head];
    entry.topic = "";
    entry.payload = "";
    entry.pending = false;
    entry.is_status = false;
    bc.queue_head = (uint8_t)((bc.queue_head + 1) % MQTT_PUBLISH_QUEUE_SIZE);
    bc.queue_count--;
  }
  bc.queue_head = 0;
  bc.queue_tail = 0;
  bc.status_queued = false;
}

bool MqttReporter::anyBrokerConnected() const {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) return true;
  }
  return false;
}

void MqttReporter::enqueueCallbackEvent(const CallbackEvent &event) {
  if (_callback_queue == nullptr) return;
  xQueueSend(_callback_queue, &event, 0);
}

void MqttReporter::processCallbackEvents() {
  if (_callback_queue == nullptr) return;
  CallbackEvent event;
  while (xQueueReceive(_callback_queue, &event, 0) == pdTRUE) {
    if (event.type == CallbackEventType::WifiDisconnected) {
      _last_wifi_disconnect_reason = event.wifi_reason;
      _wifi_got_ip = false;
      _wifi_connected_since_ms = 0;
      _wifi_last_ip = "";
      Serial.printf("MQTT reporter: WiFi disconnected, reason=%d\n",
                    _last_wifi_disconnect_reason);
      continue;
    }
    if (event.type == CallbackEventType::WifiGotIp) {
      _wifi_got_ip = true;
      _wifi_connected_since_ms = millis();
      _wifi_last_ip = WiFi.localIP().toString();
      Serial.printf("MQTT reporter: WiFi got IP: %s\n", _wifi_last_ip.c_str());
      continue;
    }
    handleMqttEvent(event);
  }
}

void MqttReporter::handleMqttEvent(const CallbackEvent &event) {
  if (event.broker_idx < 0 || event.broker_idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[event.broker_idx];
  if (event.client == nullptr || event.client != bc.client) return;

  switch (event.type) {
    case CallbackEventType::MqttConnected:
      bc.started = true;
      bc.connected = true;
      bc.connected_since_ms = millis();
      bc.next_connect_attempt_ms = 0;
      bc.connect_start_failures = 0;
      bc.online_status_pending = true;
      bc.connect_events++;
      bc.session_status_publish_count = 0;
      bc.session_packet_publish_count = 0;
      bc.last_neighbors_publish = 0;
      Serial.printf("MQTT reporter: broker %d connected\n", event.broker_idx + 1);
      break;
    case CallbackEventType::MqttDisconnected:
      Serial.printf("MQTT reporter: broker %d disconnected\n", event.broker_idx + 1);
      {
        time_t offline_time = time(nullptr);
        bc.last_offline_epoch = (_time_synced && offline_time >= MQTT_VALID_EPOCH)
                                    ? (uint32_t)offline_time
                                    : 0;
      }
      bc.connected = false;
      bc.connected_since_ms = 0;
      // The esp-mqtt client owns reconnects after start. Keep it alive and
      // let its event task retry instead of destroying it from the callback.
      bc.started = true;
      bc.online_status_pending = false;
      bc.disconnect_events++;
      break;
    case CallbackEventType::MqttError:
      Serial.printf("MQTT reporter: broker %d error type=%d\n", event.broker_idx + 1,
                    event.error_type);
      bc.connected = false;
      bc.connected_since_ms = 0;
      bc.started = true;
      bc.online_status_pending = false;
      bc.error_events++;
      break;
    default:
      break;
  }
}

bool MqttReporter::brokerNeedsTimeSync(int idx) const {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  const char *uri = _settings.broker(idx).uri;
  return strncmp(uri, "wss://", 6) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}

void MqttReporter::recordReconnectAttempt(BrokerClient &bc, uint32_t now_ms) {
  while (bc.reconnect_attempt_count > 0) {
    uint32_t oldest = bc.reconnect_attempt_ms[bc.reconnect_attempt_head];
    if ((uint32_t)(now_ms - oldest) <= RECONNECT_WINDOW_MS) break;
    bc.reconnect_attempt_head = (uint8_t)((bc.reconnect_attempt_head + 1) % MQTT_RECONNECT_RING_SIZE);
    bc.reconnect_attempt_count--;
  }

  if (bc.reconnect_attempt_count == MQTT_RECONNECT_RING_SIZE) {
    bc.reconnect_attempt_head = (uint8_t)((bc.reconnect_attempt_head + 1) % MQTT_RECONNECT_RING_SIZE);
    bc.reconnect_attempt_count--;
  }
  uint8_t insert = (uint8_t)((bc.reconnect_attempt_head + bc.reconnect_attempt_count) % MQTT_RECONNECT_RING_SIZE);
  bc.reconnect_attempt_ms[insert] = now_ms;
  bc.reconnect_attempt_count++;
}

uint32_t MqttReporter::reconnectAttemptsLastHour(const BrokerClient &bc, uint32_t now_ms) const {
  uint32_t count = 0;
  for (uint8_t i = 0; i < bc.reconnect_attempt_count; i++) {
    uint8_t index = (uint8_t)((bc.reconnect_attempt_head + i) % MQTT_RECONNECT_RING_SIZE);
    if ((uint32_t)(now_ms - bc.reconnect_attempt_ms[index]) <= RECONNECT_WINDOW_MS) {
      count++;
    }
  }
  return count;
}

void MqttReporter::upsertHeardNode(const uint8_t id[PUB_KEY_SIZE], uint32_t now_ms) {
  int match = -1;
  int free_slot = -1;
  int oldest_slot = 0;
  uint32_t oldest_age = 0;

  for (int i = 0; i < HEARD_NODE_CAPACITY; i++) {
    HeardNodeEntry &entry = _heard_nodes[i];
    if (!entry.used) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    uint32_t age = (uint32_t)(now_ms - entry.last_heard_ms);
    if (age > HEARD_NODE_WINDOW_MS) {
      entry.used = false;
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (memcmp(entry.id, id, PUB_KEY_SIZE) == 0) {
      match = i;
      break;
    }
    if (age >= oldest_age) {
      oldest_age = age;
      oldest_slot = i;
    }
  }

  int slot = match >= 0 ? match : (free_slot >= 0 ? free_slot : oldest_slot);
  memcpy(_heard_nodes[slot].id, id, PUB_KEY_SIZE);
  _heard_nodes[slot].last_heard_ms = now_ms;
  _heard_nodes[slot].used = true;
}

void MqttReporter::upsertNeighbor(const uint8_t id[PUB_KEY_SIZE], int rssi, float snr, uint32_t now_ms) {
  int slot = -1;
  int free_slot = -1;
  int oldest_slot = 0;
  uint32_t oldest_age = 0;

  for (int i = 0; i < NEIGHBOR_CAPACITY; i++) {
    NeighborEntry &entry = _neighbors[i];
    if (!entry.used) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (memcmp(entry.id, id, PUB_KEY_SIZE) == 0) {
      slot = i;
      break;
    }
    uint32_t age = (uint32_t)(now_ms - entry.last_heard_ms);
    if (age >= oldest_age) {
      oldest_age = age;
      oldest_slot = i;
    }
  }

  if (slot < 0) slot = free_slot >= 0 ? free_slot : oldest_slot;
  memcpy(_neighbors[slot].id, id, PUB_KEY_SIZE);
  _neighbors[slot].rssi = rssi;
  _neighbors[slot].snr = snr;
  _neighbors[slot].last_heard_ms = now_ms;
  _neighbors[slot].used = true;
}

uint32_t MqttReporter::heardNodesLast24Hours(uint32_t now_ms) const {
  uint32_t count = 0;
  for (int i = 0; i < HEARD_NODE_CAPACITY; i++) {
    if (_heard_nodes[i].used &&
        (uint32_t)(now_ms - _heard_nodes[i].last_heard_ms) <= HEARD_NODE_WINDOW_MS) {
      count++;
    }
  }
  return count;
}

String MqttReporter::buildNeighborsPayload(uint32_t now_ms) const {
  String payload;
  payload.reserve(32 + NEIGHBOR_CAPACITY * 110);
  payload = "{\"nodes\":[";
  bool first = true;
  for (int i = 0; i < NEIGHBOR_CAPACITY; i++) {
    const NeighborEntry &entry = _neighbors[i];
    if (!entry.used) continue;
    if (!first) payload += ",";
    first = false;
    payload += "{\"id\":\"";
    payload += bytesToHex(entry.id, PUB_KEY_SIZE);
    payload += "\",\"rssi\":";
    payload += String(entry.rssi);
    payload += ",\"snr\":";
    payload += String(entry.snr, 1);
    payload += ",\"last_seen_s\":";
    payload += String((uint32_t)(now_ms - entry.last_heard_ms) / 1000UL);
    payload += "}";
  }
  payload += "]}";
  return payload;
}

void MqttReporter::maybePublishNeighbors(uint32_t now_ms) {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    BrokerClient &bc = _clients[i];
    if (!bc.connected || bc.client == nullptr) continue;

    uint32_t interval_secs = _settings.broker(i).neighbor_interval_secs;
    if (interval_secs == 0) interval_secs = MQTT_NEIGHBOR_INTERVAL_SECS;
    if (interval_secs < MQTT_NEIGHBOR_MIN_INTERVAL_SECS) interval_secs = MQTT_NEIGHBOR_MIN_INTERVAL_SECS;
    bool due = bc.last_neighbors_publish == 0 ||
               (uint64_t)(uint32_t)(now_ms - bc.last_neighbors_publish) >= (uint64_t)interval_secs * 1000ULL;
    if (!due) continue;

    String payload = buildNeighborsPayload(now_ms);
    if (enqueuePublish(i, bc.neighbors_topic, payload, 0, false, false)) {
      bc.last_neighbors_publish = now_ms;
    }
  }
}

String MqttReporter::buildIsoTimestamp() const {
  DateTime dt(_clock->getCurrentTime());
  char buf[40];
  snprintf(
      buf,
      sizeof(buf),
      "%04d-%02d-%02dT%02d:%02d:%02dZ",
      dt.year(),
      dt.month(),
      dt.day(),
      dt.hour(),
      dt.minute(),
      dt.second());
  return String(buf);
}

String MqttReporter::buildTimeField() const {
  DateTime dt(_clock->getCurrentTime());
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

String MqttReporter::buildDateField() const {
  DateTime dt(_clock->getCurrentTime());
  char buf[16];
  snprintf(buf, sizeof(buf), "%d/%d/%d", dt.day(), dt.month(), dt.year());
  return String(buf);
}

String MqttReporter::buildRadioString() const {
  char radio_buf[64];
  const NodePrefs *prefs = _mesh->getNodePrefs();
  float freq = prefs ? prefs->freq : LORA_FREQ;
  float bw = prefs ? prefs->bw : LORA_BW;
  int sf = prefs ? prefs->sf : LORA_SF;
  int cr = prefs ? prefs->cr : LORA_CR;
  snprintf(radio_buf, sizeof(radio_buf), "SX1262 %.3f/%.0f/%d/%d", (double)freq, (double)bw, sf, cr);
  return String(radio_buf);
}

void MqttReporter::appendCpuIdleStats(String &stats) const {
  if (_idle_pct_core0 < 0.0f) {
    stats += ",\"idle_pct_core0\":null";
  } else {
    stats += ",\"idle_pct_core0\":" + String(_idle_pct_core0, 1);
  }
#if portNUM_PROCESSORS > 1
  if (_idle_pct_core1 < 0.0f) {
    stats += ",\"idle_pct_core1\":null";
  } else {
    stats += ",\"idle_pct_core1\":" + String(_idle_pct_core1, 1);
  }
#else
  stats += ",\"idle_pct_core1\":0.0";
#endif
}

String MqttReporter::buildStatusStatsPayload(int broker_idx) const {
  const uint32_t now_ms = millis();
  const bool wifi_connected = isWiFiConnected();
  String stats;
  stats.reserve(3072);
  stats = "{";
  stats += "\"uptime_ms\":" + String(now_ms);
  stats += ",\"boot_count\":" + String(_boot_count);
  stats += ",\"reset_reason\":\"" + jsonEscape(_reset_reason) + "\"";
  stats += ",\"ntp_synced\":" + String(_time_synced ? "true" : "false");
  stats += ",\"ntp_sync_age_ms\":" + String(
      _time_synced && _ntp_synced_at_ms != 0 ? (uint32_t)(now_ms - _ntp_synced_at_ms) : 0);
  stats += ",\"boot_epoch\":";
  time_t current_epoch = time(nullptr);
  uint32_t boot_epoch = 0;
  uint32_t uptime_secs = now_ms / 1000UL;
  if (_time_synced && current_epoch >= MQTT_VALID_EPOCH && current_epoch >= (time_t)uptime_secs) {
    boot_epoch = (uint32_t)(current_epoch - (time_t)uptime_secs);
  }
  stats += String(boot_epoch);
  stats += ",\"max_loop_ms\":" + String(_max_loop_ms);
  stats += ",\"max_loop_at_ms\":" + String(_max_loop_at_ms);
  stats += ",\"loop_iterations\":" + String(_loop_iterations);
  stats += ",\"wifi_reconnect_attempts\":" + String(_wifi_reconnect_attempts);
  stats += ",\"rx_publish_calls\":" + String(_rx_publish_calls);
  stats += ",\"tx_publish_calls\":" + String(_tx_publish_calls);
  stats += ",\"tx_fail_publish_calls\":" + String(_tx_fail_publish_calls);
  stats += ",\"publish_skipped_no_connection\":" + String(_publish_skipped_no_connection);
  stats += ",\"forward_successes\":" + String(_mesh->getForwardSuccessCount());
  stats += ",\"forward_successes_flood\":" + String(_mesh->getForwardFloodSuccessCount());
  stats += ",\"forward_successes_direct\":" + String(_mesh->getForwardDirectSuccessCount());
  stats += ",\"forward_failures\":" + String(_mesh->getForwardFailureCount());
  stats += ",\"tx_queue_depth\":" + String(_mesh->getTxQueueDepth());
  stats += ",\"tx_queue_depth_peak\":" + String(_mesh->getTxQueuePeakDepth());
  stats += ",\"heap_free\":" + String(ESP.getFreeHeap());
  stats += ",\"heap_min_free\":" + String(ESP.getMinFreeHeap());
  stats += ",\"heap_min_seen_since_boot\":" + String(_min_free_heap);
  stats += ",\"wifi_connected\":" + String(wifi_connected ? "true" : "false");
  stats += ",\"wifi_uptime_ms\":" + String(_wifi_connected_since_ms != 0 ? (now_ms - _wifi_connected_since_ms) : 0);
  stats += ",\"wifi_rssi\":";
  if (wifi_connected) {
    stats += String(WiFi.RSSI());
  } else {
    stats += "null";
  }
  String wifi_ssid = WiFi.SSID();
  stats += ",\"wifi_ssid\":\"" + jsonEscape(wifi_ssid.c_str()) + "\"";
  stats += ",\"nodes_heard_24h\":" + String(heardNodesLast24Hours(now_ms));
  stats += ",\"last_rx_rssi\":" + String(_last_rx_rssi);
  stats += ",\"last_rx_snr\":" + String(_last_rx_snr, 1);
  stats += ",\"tx_power_dbm\":" + String(_mesh->getTxPowerDbm());
  stats += ",\"last_tx_fail_reason\":" + String(_last_tx_fail_reason);
  stats += ",\"config_version\":" + String(MqttSettingsStore::configVersion());
  char config_crc[12];
  snprintf(config_crc, sizeof(config_crc), "0x%08lX", (unsigned long)_config_crc32);
  stats += ",\"config_crc32\":\"" + String(config_crc) + "\"";
  stats += ",\"fs_free_bytes\":";
  const size_t fs_total_bytes = SPIFFS.totalBytes();
  const size_t fs_used_bytes = SPIFFS.usedBytes();
  stats += String((unsigned long)(fs_total_bytes >= fs_used_bytes ? fs_total_bytes - fs_used_bytes : 0));
  stats += ",\"fs_total_bytes\":" + String((unsigned long)fs_total_bytes);
  nvs_stats_t nvs_stats = {};
  if (nvs_get_stats(nullptr, &nvs_stats) == ESP_OK) {
    stats += ",\"nvs_free_entries\":" + String((unsigned long)nvs_stats.free_entries);
  } else {
    stats += ",\"nvs_free_entries\":null";
  }
  stats += ",\"power_source\":\"";
  stats += board.isExternalPowered() ? "usb" : "battery";
  stats += "\"";
  stats += ",\"solar_mv\":null";
  float board_temp_c = readBoardTemperatureC();
  if (isfinite(board_temp_c)) {
    stats += ",\"board_temp_c\":" + String(board_temp_c, 1);
  } else {
    stats += ",\"board_temp_c\":null";
  }
  String channel_id = channelKeyId();
  if (channel_id.length() >= 8) {
    stats += ",\"channel_id\":\"" + jsonEscape(channel_id.substring(0, 8).c_str()) + "\"";
  } else {
    stats += ",\"channel_id\":null";
  }
  stats += ",\"git_commit\":\"" + jsonEscape(MESHCORE_GIT_COMMIT) + "\"";
  appendCpuIdleStats(stats);

  // Keep battery and airtime/utilization values sourced from the same core
  // stats path used by the CLI, rather than duplicating board/radio reads here.
  String mesh_stats = _mesh->buildMqttStatusStatsJson();
  if (mesh_stats.length() >= 2 && mesh_stats[0] == '{' && mesh_stats[mesh_stats.length() - 1] == '}') {
    String mesh_fields = mesh_stats.substring(1, mesh_stats.length() - 1);
    if (mesh_fields.length() > 0) {
      stats += ",";
      stats += mesh_fields;
    }
  }

  if (broker_idx >= 0 && broker_idx < MQTT_MAX_BROKERS) {
    const BrokerClient &bc = _clients[broker_idx];
    const MqttBrokerConfig &broker = _settings.broker(broker_idx);
    stats += ",\"mqtt\":{";
    stats += "\"broker_index\":" + String(broker_idx + 1);
    stats += ",\"connect_attempts\":" + String(bc.connect_attempts);
    stats += ",\"connect_start_failures\":" + String(bc.connect_start_failures);
    stats += ",\"connect_events\":" + String(bc.connect_events);
    stats += ",\"disconnect_events\":" + String(bc.disconnect_events);
    stats += ",\"error_events\":" + String(bc.error_events);
    String broker_uri = sanitizeBrokerUri(broker.uri);
    stats += ",\"broker_uri\":\"" + jsonEscape(broker_uri.c_str()) + "\"";
    stats += ",\"broker_username\":\"" + jsonEscape(broker.username) + "\"";
    stats += ",\"reconnect_attempts_1h\":" + String(reconnectAttemptsLastHour(bc, now_ms));
    stats += ",\"status_publishes\":" + String(bc.status_publish_count);
    stats += ",\"packet_publishes\":" + String(bc.packet_publish_count);
    stats += ",\"session_status_publishes\":" + String(bc.session_status_publish_count);
    stats += ",\"session_packet_publishes\":" + String(bc.session_packet_publish_count);
    stats += ",\"publish_failures\":" + String(bc.publish_failures);
    stats += ",\"publish_queue_depth\":" + String(bc.queue_count);
    stats += ",\"publish_queue_drops\":" + String(bc.queue_drops);
    stats += ",\"connected\":" + String(bc.connected ? "true" : "false");
    stats += ",\"uptime_ms\":" + String(bc.connected_since_ms != 0 ? (now_ms - bc.connected_since_ms) : 0);
    stats += ",\"neighbor_interval_s\":" + String(broker.neighbor_interval_secs);
    stats += ",\"last_offline_epoch\":" + String(bc.last_offline_epoch);
    stats += "}";
  }

  stats += "}";
  return stats;
}

String MqttReporter::buildStatusPayload(int broker_idx, const char *status) const {
  const MqttSharedConfig &shared = _settings.shared();
  String payload;
  payload.reserve(3800);
  payload = "{";
  if (status != nullptr && status[0] != '\0') {
    payload += "\"status\":\"" + jsonEscape(status) + "\",";
  }
  payload += "\"origin\":\"" + jsonEscape(_mesh->getNodeName()) + "\"";
  payload += ",\"origin_id\":\"" + String(_origin_id) + "\"";
  payload += ",\"model\":\"" + jsonEscape(shared.model) + "\"";
  payload += ",\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"radio\":\"" + jsonEscape(buildRadioString().c_str()) + "\"";
  payload += ",\"client_version\":\"" + jsonEscape(shared.client_version) + "\"";
  if (_time_synced) {
    payload += ",\"timestamp\":\"" + jsonEscape(buildIsoTimestamp().c_str()) + "\"";
  } else {
    payload += ",\"timestamp\":null";
  }
  payload += ",\"stats\":" + buildStatusStatsPayload(broker_idx);
  payload += "}";
  return payload;
}

String MqttReporter::buildPacketPayload(
    const char *direction,
    mesh::Packet *pkt,
    int len,
    const String &raw_hex,
    float score,
    int rssi,
    float snr,
    uint32_t duration_ms,
    bool include_radio_metrics) const {
  uint8_t packet_hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(packet_hash);
  String hash_hex = bytesToHex(packet_hash, MAX_HASH_SIZE);

  String payload;
  payload.reserve(512 + raw_hex.length());
  payload = "{";
  payload += "\"origin\":\"" + jsonEscape(_mesh->getNodeName()) + "\"";
  payload += ",\"origin_id\":\"" + String(_origin_id) + "\"";
  if (_time_synced) {
    payload += ",\"timestamp\":\"" + jsonEscape(buildIsoTimestamp().c_str()) + "\"";
  } else {
    payload += ",\"timestamp\":null";
  }
  payload += ",\"type\":\"PACKET\"";
  payload += ",\"direction\":\"" + jsonEscape(direction) + "\"";
  if (_time_synced) {
    payload += ",\"time\":\"" + jsonEscape(buildTimeField().c_str()) + "\"";
    payload += ",\"date\":\"" + jsonEscape(buildDateField().c_str()) + "\"";
  } else {
    payload += ",\"time\":null,\"date\":null";
  }
  payload += ",\"len\":\"" + String(len) + "\"";
  payload += ",\"packet_type\":\"" + String(pkt->getPayloadType()) + "\"";
  payload += ",\"route\":\"" + String(pkt->isRouteDirect() ? "D" : "F") + "\"";
  payload += ",\"payload_len\":\"" + String(pkt->payload_len) + "\"";
  payload += ",\"raw\":\"" + raw_hex + "\"";

  if (include_radio_metrics) {
    payload += ",\"SNR\":\"" + String((int)snr) + "\"";
    payload += ",\"RSSI\":\"" + String(rssi) + "\"";
    payload += ",\"score\":\"" + String((int)(score * 1000.0f)) + "\"";
    payload += ",\"duration\":\"" + String(duration_ms) + "\"";
  }

  payload += ",\"hash\":\"" + hash_hex + "\"";

  if (pkt->isRouteDirect() && shouldIncludePath(pkt)) {
    char path_buf[16];
    snprintf(path_buf, sizeof(path_buf), "%02X -> %02X", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
    payload += ",\"path\":\"" + String(path_buf) + "\"";
  }

  payload += "}";
  return payload;
}

const char *MqttReporter::getWiFiSsid() const {
  return _settings.shared().wifi_ssid;
}

bool MqttReporter::isWiFiConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool MqttReporter::isMqttConnected() const {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected) return true;
  }
  return false;
}

bool MqttReporter::isMqttConnected(int broker_idx) const {
  if (broker_idx < 0 || broker_idx >= MQTT_MAX_BROKERS) return false;
  return _clients[broker_idx].connected;
}

bool MqttReporter::getConfigValue(const char *key, char *dest, size_t dest_size, bool mask_secret) const {
  return _settings.getValue(key, dest, dest_size, mask_secret);
}

bool MqttReporter::setConfigValue(const char *key, const char *value) {
  if (!_settings.setValue(key, value)) return false;
  if (!_settings.save()) {
    // Not persisted — restore the last known-good configuration from disk.
    // (A full-store in-memory rollback copy is ~3.4 KB and, together with
    // the serializer's own frames, overflowed the 8 KB loopTask stack.)
    _settings.load();
    _config_crc32 = _settings.configCrc32();
    _identity_strings_dirty = true;
    ensureIdentityStrings();
    return false;
  }
  _config_crc32 = _settings.configCrc32();
  _identity_strings_dirty = true;
  ensureIdentityStrings();
  return true;
}

bool MqttReporter::resetConfig() {
  _settings.resetToDefaults();
  if (!_settings.save()) {
    _settings.load();
    _config_crc32 = _settings.configCrc32();
    _identity_strings_dirty = true;
    ensureIdentityStrings();
    return false;
  }
  _config_crc32 = _settings.configCrc32();
  _identity_strings_dirty = true;
  ensureIdentityStrings();
  resetAllConnections();
  return true;
}

void MqttReporter::reconnect(int broker_idx) {
  _identity_strings_dirty = true;
  ensureIdentityStrings();
  if (broker_idx >= 0 && broker_idx < MQTT_MAX_BROKERS) {
    resetBrokerConnection(broker_idx);
  } else {
    resetAllConnections();
  }
}

void MqttReporter::printBrokerConfig(Print &out, int idx) const {
  char value[160];
  char line[192];
  char key[32];

  snprintf(line, sizeof(line), "  broker %d:", idx + 1);
  out.println(line);

  snprintf(key, sizeof(key), "%d.enabled", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    enabled=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.uri", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    uri=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.username", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    username=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.password", idx + 1);
  if (getConfigValue(key, value, sizeof(value), true)) {
    snprintf(line, sizeof(line), "    password=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.topic.root", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    topic.root=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.iata", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    iata=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.retain.status", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    retain.status=%s", value);
    out.println(line);
  }
  snprintf(key, sizeof(key), "%d.neighbor.interval", idx + 1);
  if (getConfigValue(key, value, sizeof(value))) {
    snprintf(line, sizeof(line), "    neighbor.interval=%s", value);
    out.println(line);
  }

  snprintf(line, sizeof(line), "    mqtt.connected=%s", isMqttConnected(idx) ? "yes" : "no");
  out.println(line);
}

void MqttReporter::printBrokerStats(Print &out, int idx) const {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  const BrokerClient &bc = _clients[idx];
  char line[192];

  snprintf(line, sizeof(line), "  broker %d stats:", idx + 1);
  out.println(line);
  snprintf(line, sizeof(line), "    connect.attempts=%lu", (unsigned long)bc.connect_attempts);
  out.println(line);
  snprintf(line, sizeof(line), "    connect.start_failures=%lu", (unsigned long)bc.connect_start_failures);
  out.println(line);
  snprintf(line, sizeof(line), "    connect.events=%lu", (unsigned long)bc.connect_events);
  out.println(line);
  snprintf(line, sizeof(line), "    disconnect.events=%lu", (unsigned long)bc.disconnect_events);
  out.println(line);
  snprintf(line, sizeof(line), "    error.events=%lu", (unsigned long)bc.error_events);
  out.println(line);
  snprintf(line, sizeof(line), "    status.publishes=%lu", (unsigned long)bc.status_publish_count);
  out.println(line);
  snprintf(line, sizeof(line), "    packet.publishes=%lu", (unsigned long)bc.packet_publish_count);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.failures=%lu", (unsigned long)bc.publish_failures);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_depth=%u", (unsigned int)bc.queue_count);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_drops=%lu", (unsigned long)bc.queue_drops);
  out.println(line);
  snprintf(line, sizeof(line), "    connected=%s", bc.connected ? "yes" : "no");
  out.println(line);
}

void MqttReporter::printConfig(Print &out, int broker_idx) const {
  char value[160];
  char line[192];

  out.println("MQTT config:");

  if (broker_idx == -1) {
    // Print shared config
    if (getConfigValue("wifi.ssid", value, sizeof(value))) {
      snprintf(line, sizeof(line), "wifi.ssid=%s", value);
      out.println(line);
    }
    if (getConfigValue("wifi.pass", value, sizeof(value), true)) {
      snprintf(line, sizeof(line), "wifi.pass=%s", value);
      out.println(line);
    }
    if (getConfigValue("model", value, sizeof(value))) {
      snprintf(line, sizeof(line), "model=%s", value);
      out.println(line);
    }
    if (getConfigValue("client.version", value, sizeof(value))) {
      snprintf(line, sizeof(line), "client.version=%s", value);
      out.println(line);
    }
    snprintf(line, sizeof(line), "wifi.connected=%s", isWiFiConnected() ? "yes" : "no");
    out.println(line);

    // Print all brokers that are enabled or have a URI
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      const MqttBrokerConfig &b = _settings.broker(i);
      if (b.enabled || b.uri[0] != '\0') {
        printBrokerConfig(out, i);
      }
    }
  } else if (broker_idx >= 0 && broker_idx < MQTT_MAX_BROKERS) {
    printBrokerConfig(out, broker_idx);
  }
}

void MqttReporter::printStats(Print &out, int broker_idx) const {
  char line[192];
  out.println("MQTT stats:");
  snprintf(line, sizeof(line), "  uptime.ms=%lu", (unsigned long)millis());
  out.println(line);
  snprintf(line, sizeof(line), "  loop.iterations=%lu", (unsigned long)_loop_iterations);
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.reconnect_attempts=%lu", (unsigned long)_wifi_reconnect_attempts);
  out.println(line);
  snprintf(line, sizeof(line), "  rx.publish.calls=%lu", (unsigned long)_rx_publish_calls);
  out.println(line);
  snprintf(line, sizeof(line), "  tx.publish.calls=%lu", (unsigned long)_tx_publish_calls);
  out.println(line);
  snprintf(line, sizeof(line), "  tx_fail.publish.calls=%lu", (unsigned long)_tx_fail_publish_calls);
  out.println(line);
  snprintf(line, sizeof(line), "  publish.skipped.no_connection=%lu", (unsigned long)_publish_skipped_no_connection);
  out.println(line);
  snprintf(line, sizeof(line), "  heap.free=%u", (unsigned int)ESP.getFreeHeap());
  out.println(line);
  snprintf(line, sizeof(line), "  heap.min_free=%u", (unsigned int)ESP.getMinFreeHeap());
  out.println(line);
  snprintf(line, sizeof(line), "  heap.min_seen_since_boot=%u", (unsigned int)_min_free_heap);
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.connected=%s", isWiFiConnected() ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.got_ip=%s", _wifi_got_ip ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.last_disconnect_reason=%d", _last_wifi_disconnect_reason);
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.last_ip=%s", _wifi_last_ip.length() ? _wifi_last_ip.c_str() : "");
  out.println(line);

  if (broker_idx >= 0 && broker_idx < MQTT_MAX_BROKERS) {
    printBrokerStats(out, broker_idx);
    return;
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (b.enabled || b.uri[0] != '\0') {
      printBrokerStats(out, i);
    }
  }
}

void MqttReporter::maybePrintPeriodicStats() {
  unsigned long now = millis();
  if (now - _last_stats_print < MQTT_DEBUG_STATS_INTERVAL_MS) return;
  _last_stats_print = now;
  Serial.println("MQTT reporter: periodic performance snapshot");
  printStats(Serial);
}

esp_err_t MqttReporter::mqttEventHandler(esp_mqtt_event_handle_t event) {
  if (event == nullptr || event->user_context == nullptr) {
    return ESP_OK;
  }

  EventContext *ctx = static_cast<EventContext *>(event->user_context);
  CallbackEvent callback = {};
  callback.broker_idx = (int8_t)ctx->broker_idx;
  callback.client = event->client;
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      callback.type = CallbackEventType::MqttConnected;
      break;
    case MQTT_EVENT_DISCONNECTED:
      callback.type = CallbackEventType::MqttDisconnected;
      break;
    case MQTT_EVENT_ERROR:
      callback.type = CallbackEventType::MqttError;
      callback.error_type = event->error_handle ? event->error_handle->error_type : -1;
      break;
    default:
      return ESP_OK;
  }
  ctx->reporter->enqueueCallbackEvent(callback);
  return ESP_OK;
}

String MqttReporter::jsonEscape(const char *input) {
  String out;
  if (input == nullptr) return out;
  out.reserve(strlen(input) + 8);

  while (*input) {
    char c = *input++;
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

String MqttReporter::bytesToHex(const uint8_t *data, size_t len) {
  String out;
  out.reserve(len * 2);
  static const char hex_chars[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; ++i) {
    out += hex_chars[data[i] >> 4];
    out += hex_chars[data[i] & 0x0F];
  }
  return out;
}

bool MqttReporter::shouldIncludePath(const mesh::Packet *pkt) {
  if (pkt == nullptr || pkt->payload_len < 2) return false;
  uint8_t type = pkt->getPayloadType();
  return type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE || type == PAYLOAD_TYPE_TXT_MSG;
}

#endif
