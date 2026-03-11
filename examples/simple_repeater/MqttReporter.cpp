#include "MqttReporter.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include "MyMesh.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>

extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

MqttReporter::MqttReporter(MyMesh &mesh, mesh::RTCClock &clock)
    : _mesh(&mesh), _clock(&clock) {
  _last_wifi_attempt = 0;
  _last_status_publish = 0;
  _time_synced = false;
  _origin_id[0] = '\0';
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    _mqtt_clients[i] = nullptr;
    _broker_contexts[i].reporter = this;
    _broker_contexts[i].broker_index = i;
    _mqtt_started[i] = false;
    _mqtt_connected[i] = false;
    _client_ids[i][0] = '\0';
    _status_topics[i][0] = '\0';
    _packets_topics[i][0] = '\0';
  }
}

MqttReporter::~MqttReporter() {
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (_mqtt_clients[i] != nullptr) {
      esp_mqtt_client_stop(_mqtt_clients[i]);
      esp_mqtt_client_destroy(_mqtt_clients[i]);
      _mqtt_clients[i] = nullptr;
    }
  }
}

void MqttReporter::begin(FILESYSTEM *fs) {
  _settings.begin(fs);
  ensureIdentityStrings();
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_INFO);
  esp_log_level_set("TRANSPORT_BASE", ESP_LOG_INFO);
  esp_log_level_set("TRANSPORT_WS", ESP_LOG_INFO);
  esp_log_level_set("TRANS_SSL", ESP_LOG_INFO);
  esp_log_level_set("esp-tls", ESP_LOG_INFO);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  const MqttRuntimeConfig &config = _settings.config();
  Serial.printf("MQTT reporter: WiFi SSID='%s'\n", config.wifi_ssid);
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (isBrokerEnabled(i)) {
      Serial.printf("MQTT reporter: broker %d URI='%s'\n", i + 1, config.brokers[i].uri);
    }
  }

  connectWiFi();
}

void MqttReporter::loop() {
  ensureIdentityStrings();

  if (WiFi.status() != WL_CONNECTED) {
    for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
      _mqtt_connected[i] = false;
    }
    connectWiFi();
    return;
  }

  if (!_time_synced) {
    syncTimeFromNtp();
    if (mqttNeedsTimeSync() && !_time_synced) {
      return;
    }
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (!_mqtt_started[i]) {
      connectMQTT(i);
    }
  }

  if (isMqttConnected()) {
    unsigned long now = millis();
    if (now - _last_status_publish >= (unsigned long)MQTT_STATUS_INTERVAL_SECS * 1000UL) {
      publishStatus("online");
    }
  }
}

void MqttReporter::publishRxRaw(const uint8_t raw[], int len) {
  if (raw == nullptr || len <= 0) {
    _last_rx_raw = "";
    return;
  }
  _last_rx_raw = bytesToHex(raw, len);
}

void MqttReporter::publishRxPacket(mesh::Packet *pkt, int len, float score, int rssi, float snr, uint32_t duration_ms) {
  if (pkt == nullptr) return;

  String raw_hex = _last_rx_raw.length() ? _last_rx_raw : bytesToHex(pkt->payload, pkt->payload_len);
  String payload = buildPacketPayload("rx", pkt, len, raw_hex, score, rssi, snr, duration_ms, true);
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (!_mqtt_connected[i] || _mqtt_clients[i] == nullptr) continue;
    esp_mqtt_client_publish(_mqtt_clients[i], _packets_topics[i], payload.c_str(), 0, 0, 0);
  }
}

void MqttReporter::publishTxPacket(mesh::Packet *pkt, int len) {
  if (pkt == nullptr) return;

  uint8_t raw[MAX_TRANS_UNIT];
  int raw_len = pkt->writeTo(raw);
  String payload = buildPacketPayload("tx", pkt, len, bytesToHex(raw, raw_len), 0.0f, 0, 0.0f, 0, false);
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (!_mqtt_connected[i] || _mqtt_clients[i] == nullptr) continue;
    esp_mqtt_client_publish(_mqtt_clients[i], _packets_topics[i], payload.c_str(), 0, 0, 0);
  }
}

void MqttReporter::ensureIdentityStrings() {
  if (_origin_id[0] == '\0') {
    const mesh::LocalIdentity &self = _mesh->getSelfId();
    for (size_t i = 0; i < PUB_KEY_SIZE; ++i) {
      snprintf(&_origin_id[i * 2], 3, "%02X", self.pub_key[i]);
    }
    _origin_id[PUB_KEY_SIZE * 2] = '\0';
  }

  const MqttRuntimeConfig &config = _settings.config();
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    snprintf(_client_ids[i], sizeof(_client_ids[i]), "meshcore_%.*s_%d", 16, _origin_id, i + 1);
    snprintf(_status_topics[i], sizeof(_status_topics[i]), "%s/%s/%s/status", config.topic_root, config.iata, _origin_id);
    snprintf(_packets_topics[i], sizeof(_packets_topics[i]), "%s/%s/%s/packets", config.topic_root, config.iata, _origin_id);
    _offline_payloads[i] = buildStatusPayload("offline");
  }
}

void MqttReporter::resetConnections() {
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (_mqtt_clients[i] != nullptr) {
      esp_mqtt_client_stop(_mqtt_clients[i]);
      esp_mqtt_client_destroy(_mqtt_clients[i]);
      _mqtt_clients[i] = nullptr;
    }
    _mqtt_started[i] = false;
    _mqtt_connected[i] = false;
  }

  _last_wifi_attempt = 0;

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.disconnect(true, false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
}

bool MqttReporter::isBrokerEnabled(uint8_t broker_index) const {
  if (broker_index >= MQTT_MAX_BROKERS) return false;
  const MqttBrokerRuntimeConfig &broker = _settings.config().brokers[broker_index];
  return broker.enabled != 0 && broker.uri[0] != '\0';
}

bool MqttReporter::connectWiFi() {
  const MqttRuntimeConfig &config = _settings.config();
  if (WiFi.status() == WL_CONNECTED) return true;
  if (config.wifi_ssid[0] == '\0') return false;

  unsigned long now = millis();
  if (now - _last_wifi_attempt < 5000UL) return false;
  _last_wifi_attempt = now;

  Serial.printf("MQTT reporter: connecting WiFi to '%s'\n", config.wifi_ssid);
  WiFi.begin(config.wifi_ssid, config.wifi_pwd);
  return false;
}

bool MqttReporter::connectMQTT(uint8_t broker_index) {
  const MqttRuntimeConfig &runtime_config = _settings.config();
  if (broker_index >= MQTT_MAX_BROKERS) return false;
  const MqttBrokerRuntimeConfig &broker = runtime_config.brokers[broker_index];

  if (_mqtt_started[broker_index]) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!isBrokerEnabled(broker_index)) return false;
  if (mqttNeedsTimeSync() && !_time_synced) return false;

  Serial.printf(
      "MQTT reporter: init broker %u heap=%u uri='%s' user='%s'\n",
      (unsigned int)(broker_index + 1),
      (unsigned int)ESP.getFreeHeap(),
      broker.uri,
      broker.username);

  esp_mqtt_client_config_t mqtt_config = {};
  mqtt_config.uri = broker.uri;
  mqtt_config.client_id = _client_ids[broker_index];
  mqtt_config.username = broker.username;
  mqtt_config.password = broker.password;
  mqtt_config.keepalive = 60;
  mqtt_config.disable_auto_reconnect = false;
  mqtt_config.buffer_size = 2048;
  mqtt_config.out_buffer_size = 2048;
  mqtt_config.lwt_topic = _status_topics[broker_index];
  mqtt_config.lwt_msg = _offline_payloads[broker_index].c_str();
  mqtt_config.lwt_qos = 0;
  mqtt_config.lwt_retain = runtime_config.retain_status != 0;
  mqtt_config.user_context = &_broker_contexts[broker_index];
  mqtt_config.event_handle = mqttEventHandler;
  if (strncmp(broker.uri, "wss://", 6) == 0) {
    mqtt_config.crt_bundle_attach = esp_crt_bundle_attach;
  }

  _mqtt_clients[broker_index] = esp_mqtt_client_init(&mqtt_config);
  if (_mqtt_clients[broker_index] == nullptr) {
    Serial.printf("MQTT reporter: broker %u init failed\n", (unsigned int)(broker_index + 1));
    return false;
  }

  if (esp_mqtt_client_start(_mqtt_clients[broker_index]) != ESP_OK) {
    Serial.printf("MQTT reporter: broker %u start failed\n", (unsigned int)(broker_index + 1));
    esp_mqtt_client_destroy(_mqtt_clients[broker_index]);
    _mqtt_clients[broker_index] = nullptr;
    return false;
  }

  Serial.printf("MQTT reporter: broker %u started\n", (unsigned int)(broker_index + 1));
  _mqtt_started[broker_index] = true;
  return true;
}

void MqttReporter::syncTimeFromNtp() {
  configTime(0, 0, MQTT_NTP_SERVER);

  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1700000000 && millis() - start < 10000UL) {
    delay(200);
    now = time(nullptr);
  }

  if (now >= 1700000000) {
    _clock->setCurrentTime((uint32_t)now);
    _time_synced = true;
    Serial.println("MQTT reporter: NTP sync complete");
  } else if (mqttNeedsTimeSync()) {
    Serial.println("MQTT reporter: NTP sync failed");
  }
}

void MqttReporter::publishStatus(const char *status) {
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    publishStatusForBroker(i, status);
  }
  _last_status_publish = millis();
}

void MqttReporter::publishStatusForBroker(uint8_t broker_index, const char *status) {
  if (broker_index >= MQTT_MAX_BROKERS) return;
  if (!_mqtt_connected[broker_index] || _mqtt_clients[broker_index] == nullptr) return;

  String payload = buildStatusPayload(status);
  esp_mqtt_client_publish(
      _mqtt_clients[broker_index],
      _status_topics[broker_index],
      payload.c_str(),
      0,
      0,
      _settings.config().retain_status != 0);
}

void MqttReporter::handleMqttEvent(uint8_t broker_index, esp_mqtt_event_handle_t event) {
  if (broker_index >= MQTT_MAX_BROKERS) return;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      _mqtt_connected[broker_index] = true;
      Serial.printf("MQTT reporter: broker %u connected\n", (unsigned int)(broker_index + 1));
      publishStatusForBroker(broker_index, "online");
      break;
    case MQTT_EVENT_DISCONNECTED:
      Serial.printf("MQTT reporter: broker %u disconnected\n", (unsigned int)(broker_index + 1));
      _mqtt_connected[broker_index] = false;
      break;
    case MQTT_EVENT_ERROR:
      Serial.printf(
          "MQTT reporter: broker %u error type=%d\n",
          (unsigned int)(broker_index + 1),
          event->error_handle ? event->error_handle->error_type : -1);
      _mqtt_connected[broker_index] = false;
      break;
    default:
      break;
  }
}

bool MqttReporter::mqttNeedsTimeSync() const {
  const MqttRuntimeConfig &config = _settings.config();
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (!isBrokerEnabled(i)) continue;
    if (strncmp(config.brokers[i].uri, "wss://", 6) == 0) {
      return true;
    }
  }
  return false;
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

String MqttReporter::buildStatusPayload(const char *status) const {
  const MqttRuntimeConfig &config = _settings.config();
  String payload = "{";
  if (status != nullptr && status[0] != '\0') {
    payload += "\"status\":\"" + jsonEscape(status) + "\",";
  }
  payload += "\"origin\":\"" + jsonEscape(_mesh->getNodeName()) + "\"";
  payload += ",\"origin_id\":\"" + String(_origin_id) + "\"";
  payload += ",\"model\":\"" + jsonEscape(config.model) + "\"";
  payload += ",\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"radio\":\"" + jsonEscape(buildRadioString().c_str()) + "\"";
  payload += ",\"client_version\":\"" + jsonEscape(config.client_version) + "\"";
  payload += ",\"timestamp\":\"" + jsonEscape(buildIsoTimestamp().c_str()) + "\"";
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

  String payload = "{";
  payload += "\"origin\":\"" + jsonEscape(_mesh->getNodeName()) + "\"";
  payload += ",\"origin_id\":\"" + String(_origin_id) + "\"";
  payload += ",\"timestamp\":\"" + jsonEscape(buildIsoTimestamp().c_str()) + "\"";
  payload += ",\"type\":\"PACKET\"";
  payload += ",\"direction\":\"" + jsonEscape(direction) + "\"";
  payload += ",\"time\":\"" + jsonEscape(buildTimeField().c_str()) + "\"";
  payload += ",\"date\":\"" + jsonEscape(buildDateField().c_str()) + "\"";
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
  return _settings.config().wifi_ssid;
}

bool MqttReporter::isWiFiConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool MqttReporter::isMqttConnected() const {
  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    if (_mqtt_connected[i]) return true;
  }
  return false;
}

bool MqttReporter::getConfigValue(const char *key, char *dest, size_t dest_size, bool mask_secret) const {
  return _settings.getValue(key, dest, dest_size, mask_secret);
}

bool MqttReporter::setConfigValue(const char *key, const char *value) {
  if (!_settings.setValue(key, value)) return false;
  if (!_settings.save()) return false;
  ensureIdentityStrings();
  return true;
}

bool MqttReporter::resetConfig() {
  _settings.resetToDefaults();
  if (!_settings.save()) return false;
  ensureIdentityStrings();
  resetConnections();
  return true;
}

void MqttReporter::reconnect() {
  ensureIdentityStrings();
  resetConnections();
}

void MqttReporter::printConfig(Print &out) const {
  char value[160];
  char line[192];

  out.println("MQTT config:");
  if (getConfigValue("wifi.ssid", value, sizeof(value))) {
    snprintf(line, sizeof(line), "wifi.ssid=%s", value);
    out.println(line);
  }
  if (getConfigValue("wifi.pass", value, sizeof(value), true)) {
    snprintf(line, sizeof(line), "wifi.pass=%s", value);
    out.println(line);
  }
  if (getConfigValue("uri", value, sizeof(value))) {
    snprintf(line, sizeof(line), "uri=%s", value);
    out.println(line);
  }
  if (getConfigValue("username", value, sizeof(value))) {
    snprintf(line, sizeof(line), "username=%s", value);
    out.println(line);
  }
  if (getConfigValue("password", value, sizeof(value), true)) {
    snprintf(line, sizeof(line), "password=%s", value);
    out.println(line);
  }
  if (getConfigValue("topic.root", value, sizeof(value))) {
    snprintf(line, sizeof(line), "topic.root=%s", value);
    out.println(line);
  }
  if (getConfigValue("iata", value, sizeof(value))) {
    snprintf(line, sizeof(line), "iata=%s", value);
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
  if (getConfigValue("retain.status", value, sizeof(value))) {
    snprintf(line, sizeof(line), "retain.status=%s", value);
    out.println(line);
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; ++i) {
    String prefix = "broker" + String(i + 1);
    if (getConfigValue((prefix + ".enabled").c_str(), value, sizeof(value))) {
      snprintf(line, sizeof(line), "%s.enabled=%s", prefix.c_str(), value);
      out.println(line);
    }
    if (getConfigValue((prefix + ".uri").c_str(), value, sizeof(value))) {
      snprintf(line, sizeof(line), "%s.uri=%s", prefix.c_str(), value);
      out.println(line);
    }
    if (getConfigValue((prefix + ".username").c_str(), value, sizeof(value))) {
      snprintf(line, sizeof(line), "%s.username=%s", prefix.c_str(), value);
      out.println(line);
    }
    if (getConfigValue((prefix + ".password").c_str(), value, sizeof(value), true)) {
      snprintf(line, sizeof(line), "%s.password=%s", prefix.c_str(), value);
      out.println(line);
    }
    snprintf(line, sizeof(line), "%s.connected=%s", prefix.c_str(), _mqtt_connected[i] ? "yes" : "no");
    out.println(line);
  }

  snprintf(line, sizeof(line), "wifi.connected=%s", isWiFiConnected() ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "mqtt.connected=%s", isMqttConnected() ? "yes" : "no");
  out.println(line);
}

esp_err_t MqttReporter::mqttEventHandler(esp_mqtt_event_handle_t event) {
  if (event == nullptr || event->user_context == nullptr) {
    return ESP_OK;
  }

  BrokerContext *context = static_cast<BrokerContext *>(event->user_context);
  if (context->reporter == nullptr) return ESP_OK;
  context->reporter->handleMqttEvent(context->broker_index, event);
  return ESP_OK;
}

String MqttReporter::jsonEscape(const char *input) {
  String out;
  if (input == nullptr) return out;

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
  for (size_t i = 0; i < len; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

bool MqttReporter::shouldIncludePath(const mesh::Packet *pkt) {
  if (pkt == nullptr || pkt->payload_len < 2) return false;
  uint8_t type = pkt->getPayloadType();
  return type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE || type == PAYLOAD_TYPE_TXT_MSG;
}

#endif
