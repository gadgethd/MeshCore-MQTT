#include "MqttReporter.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include "MyMesh.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>

extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

MqttReporter::MqttReporter(MyMesh &mesh, mesh::RTCClock &clock)
    : _mesh(&mesh), _clock(&clock) {
  _last_wifi_attempt = 0;
  _time_synced = false;
  _origin_id[0] = '\0';
  _client_id[0] = '\0';

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    _clients[i].client = nullptr;
    _clients[i].started = false;
    _clients[i].connected = false;
    _clients[i].status_topic[0] = '\0';
    _clients[i].packets_topic[0] = '\0';
    _clients[i].last_status_publish = 0;
    _event_ctx[i].reporter = this;
    _event_ctx[i].broker_idx = i;
  }
}

MqttReporter::~MqttReporter() {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].client != nullptr) {
      esp_mqtt_client_stop(_clients[i].client);
      esp_mqtt_client_destroy(_clients[i].client);
      _clients[i].client = nullptr;
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

  const MqttSharedConfig &shared = _settings.shared();
  Serial.printf("MQTT reporter: WiFi SSID='%s'\n", shared.wifi_ssid);
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (b.enabled && b.uri[0] != '\0') {
      Serial.printf("MQTT reporter: broker %d URI='%s'\n", i + 1, b.uri);
    }
  }
  connectWiFi();
}

void MqttReporter::loop() {
  ensureIdentityStrings();

  if (WiFi.status() != WL_CONNECTED) {
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      _clients[i].connected = false;
    }
    connectWiFi();
    return;
  }

  // Check if any enabled broker needs NTP
  bool any_needs_sync = false;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_settings.broker(i).enabled && brokerNeedsTimeSync(i)) {
      any_needs_sync = true;
      break;
    }
  }

  if (!_time_synced && any_needs_sync) {
    syncTimeFromNtp();
    if (!_time_synced) return;
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
  unsigned long now = millis();
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected) {
      if (now - _clients[i].last_status_publish >= (unsigned long)MQTT_STATUS_INTERVAL_SECS * 1000UL) {
        publishStatus(i, "online");
      }
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

  bool any_connected = false;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      any_connected = true;
      break;
    }
  }
  if (!any_connected) return;

  String raw_hex = _last_rx_raw.length() ? _last_rx_raw : bytesToHex(pkt->payload, pkt->payload_len);
  String payload = buildPacketPayload("rx", pkt, len, raw_hex, score, rssi, snr, duration_ms, true);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      esp_mqtt_client_publish(_clients[i].client, _clients[i].packets_topic, payload.c_str(), 0, 0, 0);
    }
  }
}

void MqttReporter::publishTxPacket(mesh::Packet *pkt, int len) {
  if (pkt == nullptr) return;

  bool any_connected = false;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      any_connected = true;
      break;
    }
  }
  if (!any_connected) return;

  uint8_t raw[MAX_TRANS_UNIT];
  int raw_len = pkt->writeTo(raw);
  String payload = buildPacketPayload("tx", pkt, len, bytesToHex(raw, raw_len), 0.0f, 0, 0.0f, 0, false);

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      esp_mqtt_client_publish(_clients[i].client, _clients[i].packets_topic, payload.c_str(), 0, 0, 0);
    }
  }
}

void MqttReporter::ensureIdentityStrings() {
  if (_origin_id[0] == '\0') {
    const mesh::LocalIdentity &self = _mesh->getSelfId();
    for (size_t i = 0; i < PUB_KEY_SIZE; ++i) {
      snprintf(&_origin_id[i * 2], 3, "%02X", self.pub_key[i]);
    }
    _origin_id[PUB_KEY_SIZE * 2] = '\0';

    snprintf(_client_id, sizeof(_client_id), "meshcore_%.*s", 16, _origin_id);
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    if (!b.enabled) continue;
    snprintf(_clients[i].status_topic, sizeof(_clients[i].status_topic),
             "%s/%s/%s/status", b.topic_root, b.iata, _origin_id);
    snprintf(_clients[i].packets_topic, sizeof(_clients[i].packets_topic),
             "%s/%s/%s/packets", b.topic_root, b.iata, _origin_id);
    _clients[i].offline_payload = buildStatusPayload("offline");
  }
}

void MqttReporter::resetBrokerConnection(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (bc.client != nullptr) {
    esp_mqtt_client_stop(bc.client);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
  }
  bc.started = false;
  bc.connected = false;
}

void MqttReporter::resetAllConnections() {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    resetBrokerConnection(i);
  }
  _last_wifi_attempt = 0;

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
  if (now - _last_wifi_attempt < 5000UL) return false;
  _last_wifi_attempt = now;

  Serial.printf("MQTT reporter: connecting WiFi to '%s'\n", shared.wifi_ssid);
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

  // Use a unique client_id per broker
  char broker_client_id[48];
  snprintf(broker_client_id, sizeof(broker_client_id), "%s_%d", _client_id, idx + 1);

  Serial.printf(
      "MQTT reporter: init broker %d heap=%u uri='%s' user='%s'\n",
      idx + 1,
      (unsigned int)ESP.getFreeHeap(),
      broker.uri,
      broker.username);

  esp_mqtt_client_config_t mqtt_config = {};
  mqtt_config.uri = broker.uri;
  mqtt_config.client_id = broker_client_id;
  mqtt_config.username = broker.username;
  mqtt_config.password = broker.password;
  mqtt_config.keepalive = 60;
  mqtt_config.disable_auto_reconnect = false;
  mqtt_config.buffer_size = 2048;
  mqtt_config.out_buffer_size = 2048;
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
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_init failed\n", idx + 1);
    return false;
  }

  if (esp_mqtt_client_start(bc.client) != ESP_OK) {
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_start failed\n", idx + 1);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
    return false;
  }

  Serial.printf("MQTT reporter: broker %d MQTT client started\n", idx + 1);
  bc.started = true;
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
  } else {
    Serial.println("MQTT reporter: NTP sync failed");
  }
}

void MqttReporter::publishStatus(int idx, const char *status) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) return;

  String payload = buildStatusPayload(status);
  esp_mqtt_client_publish(bc.client, bc.status_topic, payload.c_str(), 0, 0,
                          _settings.broker(idx).retain_status != 0);
  bc.last_status_publish = millis();
}

void MqttReporter::handleMqttEvent(int broker_idx, esp_mqtt_event_handle_t event) {
  if (broker_idx < 0 || broker_idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[broker_idx];

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      bc.connected = true;
      Serial.printf("MQTT reporter: broker %d connected\n", broker_idx + 1);
      publishStatus(broker_idx, "online");
      break;
    case MQTT_EVENT_DISCONNECTED:
      Serial.printf("MQTT reporter: broker %d disconnected\n", broker_idx + 1);
      bc.connected = false;
      break;
    case MQTT_EVENT_ERROR:
      Serial.printf("MQTT reporter: broker %d error type=%d\n", broker_idx + 1,
                     event->error_handle ? event->error_handle->error_type : -1);
      bc.connected = false;
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
  const MqttSharedConfig &shared = _settings.shared();
  String payload = "{";
  if (status != nullptr && status[0] != '\0') {
    payload += "\"status\":\"" + jsonEscape(status) + "\",";
  }
  payload += "\"origin\":\"" + jsonEscape(_mesh->getNodeName()) + "\"";
  payload += ",\"origin_id\":\"" + String(_origin_id) + "\"";
  payload += ",\"model\":\"" + jsonEscape(shared.model) + "\"";
  payload += ",\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"radio\":\"" + jsonEscape(buildRadioString().c_str()) + "\"";
  payload += ",\"client_version\":\"" + jsonEscape(shared.client_version) + "\"";
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
  if (!_settings.save()) return false;
  ensureIdentityStrings();
  return true;
}

bool MqttReporter::resetConfig() {
  _settings.resetToDefaults();
  if (!_settings.save()) return false;
  ensureIdentityStrings();
  resetAllConnections();
  return true;
}

void MqttReporter::reconnect(int broker_idx) {
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

  snprintf(line, sizeof(line), "    mqtt.connected=%s", isMqttConnected(idx) ? "yes" : "no");
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

esp_err_t MqttReporter::mqttEventHandler(esp_mqtt_event_handle_t event) {
  if (event == nullptr || event->user_context == nullptr) {
    return ESP_OK;
  }

  EventContext *ctx = static_cast<EventContext *>(event->user_context);
  ctx->reporter->handleMqttEvent(ctx->broker_idx, event);
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
