#include "MqttReporter.h"

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include "MyMesh.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <SPIFFS.h>
#include <math.h>

// Prefer the SNTP status API when the installed IDF exposes it. The pinned
// Arduino/IDF build leaves SNTP_CHECK_RESPONSE at its default of zero, so its
// completion is intentionally reported as an explicit no-validation mode.
#if defined(__has_include)
#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#define MQTT_HAVE_SNTP_STATUS 1
#elif __has_include(<lwip/apps/esp_sntp.h>)
#include <lwip/apps/esp_sntp.h>
#define MQTT_HAVE_SNTP_STATUS 1
#endif
#endif

#if defined(__has_include)
#if __has_include(<lwip/apps/sntp_opts.h>)
#include <lwip/apps/sntp_opts.h>
#endif
#if !defined(MQTT_HAVE_SNTP_STATUS) && __has_include(<lwip/apps/sntp.h>)
#include <lwip/apps/sntp.h>
#define MQTT_HAVE_SNTP_CONTROL 1
#endif
#endif

#if defined(SNTP_CHECK_RESPONSE) && SNTP_CHECK_RESPONSE >= 2 && defined(MQTT_HAVE_SNTP_STATUS)
#define MQTT_SNTP_RESPONSE_VALIDATED 1
#else
#define MQTT_SNTP_RESPONSE_VALIDATED 0
#endif

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
constexpr unsigned long MQTT_NTP_TIMEOUT_MS = 10000UL;
static_assert(MQTT_MAX_PUBLISHES_PER_LOOP <= mqtt_publish::kDefaultMaxPublishesPerPass,
              "the reporter loop publish-call budget exceeds the tested policy");
static_assert(MAX_TRANS_UNIT <= UINT8_MAX, "packet wire length no longer fits writeTo()");
static_assert(MAX_PACKET_PAYLOAD == 184, "revisit MQTT packet payload-size assertions");
static_assert(MAX_PATH_SIZE == 64, "revisit MQTT path-size assertions");

// NTP retry backoff: 30 s, 60 s, 120 s, 240 s (capped), by consecutive failures.
// (Not constexpr: the statement body is not allowed under the ESP toolchain's
// gnu++11 mode.)
inline unsigned long ntpRetryDelayMs(uint8_t failures) {
  return mqtt_clock::retryDelayMs(failures);
}

// Cap on esp-mqtt's outgoing message queue (QoS0 publishes). If the uplink
// stalls the outbox grows without bound and can starve the heap; QoS0 is
// lossy by contract, so drop new publishes once the high-water mark is hit.
#if defined(BOARD_HAS_PSRAM)
constexpr int MQTT_OUTBOX_HIGH_WATER = 16 * 1024;
#else
constexpr int MQTT_OUTBOX_HIGH_WATER = 8 * 1024;
#endif
constexpr uint32_t MQTT_CONNECT_RETRY_MAX_MS = 300000UL;

// Heap budget for TLS sessions: use the same two-slot non-PSRAM and six-slot
// PSRAM limits as before, but select the budget from runtime psramFound().
constexpr uint8_t MQTT_MAX_ACTIVE_TLS_NO_PSRAM = 2;
constexpr uint8_t MQTT_MAX_ACTIVE_TLS_PSRAM = MQTT_MAX_BROKERS;
constexpr time_t MQTT_VALID_EPOCH = (time_t)mqtt_clock::kMinimumTrustworthyEpoch;

#if MQTT_SNTP_RESPONSE_VALIDATED
constexpr mqtt_clock::ValidationMode MQTT_NTP_VALIDATION_MODE =
    mqtt_clock::ValidationMode::ValidatedExchange;
#else
constexpr mqtt_clock::ValidationMode MQTT_NTP_VALIDATION_MODE =
    mqtt_clock::ValidationMode::ExplicitNoValidation;
#endif

uint32_t timeToEpoch(time_t value) {
  if (value <= 0) return 0;
  const uint64_t epoch = (uint64_t)value;
  return epoch > UINT32_MAX ? 0 : (uint32_t)epoch;
}

int reporterBrokerConfigKey(const char *key, const char **field_out) {
  if (key == nullptr || field_out == nullptr) return -1;
  if (strncmp(key, "mqtt.", 5) == 0) key += 5;
  if (key[0] >= '1' && key[0] <= '0' + MQTT_MAX_BROKERS && key[1] == '.') {
    *field_out = key + 2;
    return key[0] - '1';
  }
  if (strcmp(key, "wifi.ssid") == 0 || strcmp(key, "wifi.pass") == 0 ||
      strcmp(key, "model") == 0 || strcmp(key, "client.version") == 0 ||
      strcmp(key, "boot_count") == 0) {
    return -1;
  }
  *field_out = key;
  return 0;
}

bool reporterConfigNeedsClientRestart(const char *key, int *broker_idx_out) {
  const char *field = nullptr;
  int broker_idx = reporterBrokerConfigKey(key, &field);
  if (broker_idx < 0 || field == nullptr) return false;
  if (broker_idx_out != nullptr) *broker_idx_out = broker_idx;
  // All connection/LWT fields are copied into esp-mqtt at init. The neighbor
  // cadence is read by the reporter loop and does not require a restart.
  return strcmp(field, "neighbor.interval") != 0;
}

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
  _ntp_attempt_generation = 0;
  _ntp_pending_generation = 0;
  _ntp_failures = 0;
  _ntp_sync_pending = false;
  _time_synced = false;
  _ntp_synced_at_ms = 0;
  _clock_source = mqtt_clock::Source::None;
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
  _build_failures = 0;
  _serialize_preflight_drops = 0;
  _publish_queue_bytes_total = 0;
  _build_buffer = nullptr;
  _psram_available = false;
  _next_publish_broker = 0;
  _last_rx_raw_len = 0;
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
  mqtt_reconnect::resetGuard(_reconnect_guard);
  _pending_wifi_event.store(0, std::memory_order_relaxed);
  _pending_wifi_disconnect_reason.store(-1, std::memory_order_relaxed);
  _wifi_callback_queue_drops.store(0, std::memory_order_relaxed);
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
    _clients[i].client_generation.store(0, std::memory_order_relaxed);
    _clients[i].pending_connected_generation.store(0, std::memory_order_relaxed);
    _clients[i].pending_terminal_generation.store(0, std::memory_order_relaxed);
    _clients[i].callback_queue_drops.store(0, std::memory_order_relaxed);
    _clients[i].pending_terminal_type.store(0, std::memory_order_relaxed);
    _clients[i].pending_error_type.store(-1, std::memory_order_relaxed);
    _clients[i].pending_error_code.store(-1, std::memory_order_relaxed);
    _clients[i].pending_tls_last_error.store(-1, std::memory_order_relaxed);
    _clients[i].pending_tls_stack_error.store(-1, std::memory_order_relaxed);
    _clients[i].pending_tls_cert_verify_flags.store(-1, std::memory_order_relaxed);
    _clients[i].pending_transport_sock_errno.store(-1, std::memory_order_relaxed);
    _clients[i].pending_connect_return_code.store(-1, std::memory_order_relaxed);
    _clients[i].started = false;
    _clients[i].connected_since_ms = 0;
    _clients[i].connected = false;
    _clients[i].effective_transport = mqtt_tls_cap::Transport::Unknown;
    _clients[i].next_connect_attempt_ms = 0;
    _clients[i].status_topic[0] = '\0';
    _clients[i].packets_topic[0] = '\0';
    _clients[i].neighbors_topic[0] = '\0';
    _clients[i].offline_payload = nullptr;
    _clients[i].offline_payload_len = 0;
    _clients[i].last_status_publish = 0;
    _clients[i].last_neighbors_publish = 0;
    _clients[i].queue_head = 0;
    _clients[i].queue_tail = 0;
    _clients[i].queue_count = 0;
    _clients[i].queue_bytes = 0;
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
    _clients[i].queue_byte_drops = 0;
    _clients[i].outbox_drops = 0;
    memset(_clients[i].reconnect_attempt_ms, 0, sizeof(_clients[i].reconnect_attempt_ms));
    _clients[i].reconnect_attempt_head = 0;
    _clients[i].reconnect_attempt_count = 0;
    _clients[i].last_offline_epoch = 0;
    _clients[i].heap_inactive = false;
    _clients[i].last_error_type = -1;
    _clients[i].last_error_code = -1;
    _clients[i].last_tls_last_error = -1;
    _clients[i].last_tls_stack_error = -1;
    _clients[i].last_tls_cert_verify_flags = -1;
    _clients[i].last_transport_sock_errno = -1;
    _clients[i].last_connect_return_code = -1;
    _clients[i].last_error_at_ms = 0;
    mqtt_reconnect::reset(_clients[i].recon);
    _clients[i].recon_seeded = false;
    for (uint8_t q = 0; q < MQTT_PUBLISH_QUEUE_SIZE; q++) {
      _clients[i].publish_queue[q].topic = nullptr;
      _clients[i].publish_queue[q].payload = nullptr;
      _clients[i].publish_queue[q].topic_len = 0;
      _clients[i].publish_queue[q].payload_len = 0;
      _clients[i].publish_queue[q].copied_bytes = 0;
      _clients[i].publish_queue[q].qos = 0;
      _clients[i].publish_queue[q].retain = false;
      _clients[i].publish_queue[q].is_status = false;
      _clients[i].publish_queue[q].pending = false;
    }
    _event_ctx[i].reporter = this;
    _event_ctx[i].broker_idx = i;
    _event_ctx[i].generation = 0;
  }

  // WiFi callbacks are registered in begin(), after construction is complete.
  s_instance.store(this, std::memory_order_release);
}

MqttReporter::~MqttReporter() {
  MqttReporter *expected = this;
  s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    clearPublishQueue(i);
    if (_clients[i].client != nullptr) {
      esp_mqtt_client_stop(_clients[i].client);
      esp_mqtt_client_destroy(_clients[i].client);
      _clients[i].client = nullptr;
      _clients[i].effective_transport = mqtt_tls_cap::Transport::Unknown;
    }
    releaseReporterBuffer(_clients[i].offline_payload);
    _clients[i].offline_payload = nullptr;
    _clients[i].offline_payload_len = 0;
  }
  releaseReporterBuffer(_build_buffer);
  _build_buffer = nullptr;
  if (_callback_queue != nullptr) {
    vQueueDelete(_callback_queue);
    _callback_queue = nullptr;
  }
}

void MqttReporter::begin(FILESYSTEM *fs) {
  _settings.begin(fs);
  _psram_available = psramFound();
  _build_buffer = allocateReporterBuffer(mqtt_publish::kBuildBufferBytes, true);
  if (_build_buffer == nullptr) {
    Serial.println("MQTT reporter: checked payload buffer allocation failed; publishes disabled");
  } else {
    Serial.printf("MQTT reporter: %u-byte checked payload buffer allocated (%s preference)\n",
                  (unsigned int)mqtt_publish::kBuildBufferBytes,
                  _psram_available ? "PSRAM" : "internal");
  }
  _config_crc32 = _settings.configCrc32();
  _boot_count = _settings.incrementBootCount();
  StrHelper::strncpy(_reset_reason, resetReasonString((int)esp_reset_reason()), sizeof(_reset_reason));
  ensureIdentityStrings();
  _callback_queue = xQueueCreate(MQTT_CALLBACK_QUEUE_DEPTH, sizeof(CallbackEvent));
  if (_callback_queue == nullptr) {
    Serial.println("MQTT reporter: callback queue allocation failed; terminal fallback active");
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
    callback.broker_idx = -1;
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
      reconcileMqttTerminal(i, (uint32_t)now);
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

  if (mqtt_reporting_enabled) {
    checkNtpSyncComplete();
    if (mqtt_clock::attemptDue(
            _ntp_attempted,
            _ntp_sync_pending,
            _clock_source,
            (uint32_t)now,
            (uint32_t)_last_ntp_attempt,
            _ntp_failures)) {
      _last_ntp_attempt = now;
      _ntp_attempted = true;
      syncTimeFromNtp();
    }
  }

  // Connect/maintain all enabled brokers. Reconnects are scheduled here
  // through MqttReconnectPolicy (ladder backoff, stability gate, circuit
  // breaker, per-slot stagger); esp-mqtt's internal retry loop is disabled.
  processBrokerReconnects((uint32_t)now);

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

  mqtt_publish::DrainState drain = mqtt_publish::startDrain(
      (uint32_t)(esp_timer_get_time() / 1000));
  for (int offset = 0; offset < MQTT_MAX_BROKERS; offset++) {
    const int i = (_next_publish_broker + offset) % MQTT_MAX_BROKERS;
    if (_clients[i].connected) {
      drainPublishQueue(i, drain);
    } else {
      clearPublishQueue(i);
    }
    if (!mqtt_publish::mayPublish(
            drain,
            (uint32_t)(esp_timer_get_time() / 1000),
            MQTT_PUBLISHES_PER_LOOP,
            mqtt_publish::kDrainElapsedBudgetMs)) {
      break;
    }
  }
  _next_publish_broker = mqtt_publish::nextBroker(
      _next_publish_broker, MQTT_MAX_BROKERS);

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
  if (raw == nullptr || len <= 0 || len > MAX_TRANS_UNIT) {
    _last_rx_raw_len = 0;
    return;
  }
  memcpy(_last_rx_raw, raw, (size_t)len);
  _last_rx_raw_len = (size_t)len;
}

void MqttReporter::clearPendingRxRaw() {
  _last_rx_raw_len = 0;
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

  if (!anyBrokerConnected()) {
    _last_rx_raw_len = 0;
    _publish_skipped_no_connection++;
    return;
  }

  const uint8_t *raw = _last_rx_raw_len > 0 ? _last_rx_raw : pkt->payload;
  const size_t raw_len = _last_rx_raw_len > 0 ? _last_rx_raw_len : pkt->payload_len;
  size_t payload_len = 0;
  if (!buildPacketPayload("rx", pkt, len, raw, raw_len,
                          score, rssi, snr, duration_ms, true, payload_len)) {
    _last_rx_raw_len = 0;
    _build_failures++;
    return;
  }
  _last_rx_raw_len = 0;

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, _build_buffer, payload_len,
                     0, false, false);
    }
  }
}

void MqttReporter::publishTxPacket(mesh::Packet *pkt, int len) {
  if (pkt == nullptr) return;
  _tx_publish_calls++;

  uint8_t raw[MAX_TRANS_UNIT];
  const mqtt_wire::Preflight preflight = mqtt_wire::validate(
      pkt->payload_len,
      pkt->path_len,
      pkt->hasTransportCodes(),
      MAX_PACKET_PAYLOAD,
      MAX_PATH_SIZE,
      sizeof(raw));
  if (!preflight.ok()) {
    _serialize_preflight_drops++;
    return;
  }

  if (!anyBrokerConnected()) {
    _publish_skipped_no_connection++;
    return;
  }

  const size_t raw_len = pkt->writeTo(raw);
  if (raw_len != preflight.raw_bytes) {
    _serialize_preflight_drops++;
    return;
  }
  size_t payload_len = 0;
  if (!buildPacketPayload("tx", pkt, len, raw, raw_len,
                          0.0f, 0, 0.0f, 0, false, payload_len)) {
    _build_failures++;
    return;
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, _build_buffer, payload_len,
                     0, false, false);
    }
  }
}

void MqttReporter::publishTxFail(mesh::Packet *pkt, int len, int reason) {
  if (pkt == nullptr) return;
  _tx_fail_publish_calls++;
  _last_tx_fail_reason = reason;

  uint8_t raw[MAX_TRANS_UNIT];
  const mqtt_wire::Preflight preflight = mqtt_wire::validate(
      pkt->payload_len,
      pkt->path_len,
      pkt->hasTransportCodes(),
      MAX_PACKET_PAYLOAD,
      MAX_PATH_SIZE,
      sizeof(raw));
  if (!preflight.ok()) {
    _serialize_preflight_drops++;
    return;
  }

  if (!anyBrokerConnected()) {
    _publish_skipped_no_connection++;
    return;
  }

  const size_t raw_len = pkt->writeTo(raw);
  if (raw_len != preflight.raw_bytes) {
    _serialize_preflight_drops++;
    return;
  }
  size_t payload_len = 0;
  if (!buildPacketPayload("tx_fail", pkt, len, raw, raw_len,
                          0.0f, 0, 0.0f, 0, false, payload_len)) {
    _build_failures++;
    return;
  }

  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) {
      enqueuePublish(i, _clients[i].packets_topic, _build_buffer, payload_len,
                     0, false, false);
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
      releaseReporterBuffer(_clients[i].offline_payload);
      _clients[i].offline_payload = nullptr;
      _clients[i].offline_payload_len = 0;
      continue;
    }
    String status_topic = buildStatusTopicPath(b.topic_root, b.iata, _origin_id);
    String packets_topic = buildPacketsTopicPath(b.topic_root, b.iata, _origin_id);
    String neighbors_topic = buildNeighborsTopicPath(b.topic_root, b.iata, _origin_id);
    if (status_topic.length() >= sizeof(_clients[i].status_topic) ||
        packets_topic.length() >= sizeof(_clients[i].packets_topic) ||
        neighbors_topic.length() >= sizeof(_clients[i].neighbors_topic)) {
      _clients[i].status_topic[0] = '\0';
      _clients[i].packets_topic[0] = '\0';
      _clients[i].neighbors_topic[0] = '\0';
      releaseReporterBuffer(_clients[i].offline_payload);
      _clients[i].offline_payload = nullptr;
      _clients[i].offline_payload_len = 0;
      _build_failures++;
      continue;
    }
    StrHelper::strncpy(_clients[i].status_topic, status_topic.c_str(), sizeof(_clients[i].status_topic));
    StrHelper::strncpy(_clients[i].packets_topic, packets_topic.c_str(), sizeof(_clients[i].packets_topic));
    StrHelper::strncpy(_clients[i].neighbors_topic, neighbors_topic.c_str(), sizeof(_clients[i].neighbors_topic));
    size_t offline_payload_len = 0;
    if (!buildStatusPayload(i, "offline", offline_payload_len) ||
        offline_payload_len > mqtt_publish::kMaxLwtPayloadBytes ||
        !replaceOfflinePayload(_clients[i], _build_buffer, offline_payload_len)) {
      releaseReporterBuffer(_clients[i].offline_payload);
      _clients[i].offline_payload = nullptr;
      _clients[i].offline_payload_len = 0;
      _build_failures++;
    }
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
  bc.effective_transport = mqtt_tls_cap::Transport::Unknown;
  bc.pending_connected_generation.store(0, std::memory_order_release);
  bc.pending_terminal_generation.store(0, std::memory_order_release);
  bc.pending_terminal_type.store(0, std::memory_order_release);
  bc.pending_error_type.store(-1, std::memory_order_release);
  bc.pending_error_code.store(-1, std::memory_order_release);
  bc.pending_tls_last_error.store(-1, std::memory_order_release);
  bc.pending_tls_stack_error.store(-1, std::memory_order_release);
  bc.pending_tls_cert_verify_flags.store(-1, std::memory_order_release);
  bc.pending_transport_sock_errno.store(-1, std::memory_order_release);
  bc.pending_connect_return_code.store(-1, std::memory_order_release);
  clearPublishQueue(idx);
  bc.started = false;
  bc.connected = false;
  bc.online_status_pending = false;
  bc.next_connect_attempt_ms = 0;
  // Deliberate reset (mqtt reconnect / config reset): retry immediately with
  // a fresh ladder; no boot-style stagger for this slot.
  mqtt_reconnect::reset(bc.recon);
  bc.recon_seeded = true;
}

void MqttReporter::resetAllConnections() {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    resetBrokerConnection(i);
  }
  mqtt_reconnect::resetGuard(_reconnect_guard);
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

bool MqttReporter::tlsBudgetAllows(int broker_idx, mqtt_tls_cap::Transport requested) const {
  (void)broker_idx;
  mqtt_tls_cap::Slot slots[MQTT_MAX_BROKERS] = {};
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const BrokerClient &bc = _clients[i];
    slots[i].configured_enabled = _settings.broker(i).enabled != 0;
    if (i == broker_idx && bc.client != nullptr) {
      // connectMQTT() destroys this handle before creating its replacement;
      // do not count that same live resource as an additional allocation.
      slots[i].client_state = mqtt_tls_cap::ClientState::None;
    } else if (bc.client == nullptr) {
      slots[i].client_state = mqtt_tls_cap::ClientState::None;
    } else if (bc.connected) {
      slots[i].client_state = mqtt_tls_cap::ClientState::Connected;
    } else if (bc.started) {
      slots[i].client_state = mqtt_tls_cap::ClientState::Connecting;
    } else {
      // A handle which is not currently connected still owns its transport
      // until the owner task destroys it.
      slots[i].client_state = mqtt_tls_cap::ClientState::Disconnected;
    }
    slots[i].effective_transport = bc.effective_transport;
  }

  return mqtt_tls_cap::canStart(
      requested,
      slots,
      MQTT_MAX_BROKERS,
      psramFound(),
      MQTT_MAX_ACTIVE_TLS_NO_PSRAM,
      MQTT_MAX_ACTIVE_TLS_PSRAM);
}

uint8_t MqttReporter::liveTlsCount() const {
  mqtt_tls_cap::Slot slots[MQTT_MAX_BROKERS] = {};
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const BrokerClient &bc = _clients[i];
    slots[i].configured_enabled = _settings.broker(i).enabled != 0;
    if (bc.client == nullptr) {
      slots[i].client_state = mqtt_tls_cap::ClientState::None;
    } else if (bc.connected) {
      slots[i].client_state = mqtt_tls_cap::ClientState::Connected;
    } else if (bc.started) {
      slots[i].client_state = mqtt_tls_cap::ClientState::Connecting;
    } else {
      slots[i].client_state = mqtt_tls_cap::ClientState::Disconnected;
    }
    slots[i].effective_transport = bc.effective_transport;
  }
  return mqtt_tls_cap::liveTlsCount(slots, MQTT_MAX_BROKERS);
}

uint8_t MqttReporter::tlsCap() const {
  return mqtt_tls_cap::maxActiveTls(
      psramFound(), MQTT_MAX_ACTIVE_TLS_NO_PSRAM, MQTT_MAX_ACTIVE_TLS_PSRAM);
}

void MqttReporter::processBrokerReconnects(uint32_t now_ms) {
  bool attempted_this_pass = false;
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &b = _settings.broker(i);
    BrokerClient &bc = _clients[i];
    if (!b.enabled || b.uri[0] == '\0') {
      if (bc.client != nullptr) {
        Serial.printf("MQTT reporter: broker %d disabled; stopping live client before apply\n", i + 1);
        resetBrokerConnection(i);
      }
      if (bc.recon_seeded) {
        // Disabled: drop any pending schedule so a re-enable starts fresh.
        mqtt_reconnect::reset(bc.recon);
        bc.recon_seeded = false;
      }
      continue;
    }
    if (!bc.recon_seeded) {
      mqtt_reconnect::seedInitial(bc.recon, now_ms, (uint8_t)i);
      bc.recon_seeded = true;
    }
    mqtt_reconnect::onTick(bc.recon, now_ms);
    if (mqtt_reconnect::reconcileAttemptTimeout(bc.recon, now_ms, (uint8_t)i)) {
      bc.connected = false;
      bc.connected_since_ms = 0;
      bc.online_status_pending = false;
      Serial.printf("MQTT reporter: broker %d attempt timed out; retry in %lu ms\n",
                    i + 1,
                    (unsigned long)mqtt_reconnect::nextWaitMs(bc.recon, now_ms));
    }

    if (!bc.started) {
      // Heap budget is based on the runtime PSRAM result and on every live
      // client's recorded transport, including connecting/disconnected
      // handles. Stored URI edits cannot reclassify an allocated client.
      const mqtt_tls_cap::Transport requested_transport =
          mqtt_tls_cap::transportForUri(b.uri);
      if (mqtt_tls_cap::countsAgainstTlsBudget(requested_transport) &&
          !tlsBudgetAllows(i, requested_transport)) {
        const uint8_t tls_cap = mqtt_tls_cap::maxActiveTls(
            psramFound(), MQTT_MAX_ACTIVE_TLS_NO_PSRAM, MQTT_MAX_ACTIVE_TLS_PSRAM);
        if (!bc.heap_inactive) {
          bc.heap_inactive = true;
          Serial.printf("MQTT reporter: broker %d inactive (heap budget: max %u live TLS brokers)\n",
                        i + 1, (unsigned int)tls_cap);
        }
        if (!bc.recon.attempt_pending) {
          bc.recon.attempt_pending = true;
          bc.recon.next_attempt_ms = now_ms + 60000UL;
        }
        continue;
      }
      bc.heap_inactive = false;
      if (mqtt_reconnect::attemptDue(bc.recon, now_ms) &&
          mqtt_reconnect::guardAllowsAttempt(_reconnect_guard, now_ms, attempted_this_pass) &&
          connectMQTT(i)) {
        mqtt_reconnect::noteAttempt(_reconnect_guard, now_ms);
        attempted_this_pass = true;
      }
      continue;
    }

    if (!bc.connected && bc.client != nullptr && mqtt_reconnect::attemptDue(bc.recon, now_ms) &&
        mqtt_reconnect::guardAllowsAttempt(_reconnect_guard, now_ms, attempted_this_pass)) {
      bc.connect_attempts++;
      recordReconnectAttempt(bc, now_ms);
      mqtt_reconnect::onAttemptStarted(bc.recon, now_ms);
      mqtt_reconnect::noteAttempt(_reconnect_guard, now_ms);
      attempted_this_pass = true;
      Serial.printf("MQTT reporter: broker %d reconnect attempt (rung %u%s)\n",
                    i + 1, (unsigned int)bc.recon.rung,
                    bc.recon.breaker ? ", breaker probe" : "");
      if (esp_mqtt_client_reconnect(bc.client) != ESP_OK) {
        // The client only accepts a manual reconnect from its parked
        // "waiting for reconnect" state; force a stop/start restart if it is
        // in any other state.
        Serial.printf("MQTT reporter: broker %d forced restart (stop/start)\n", i + 1);
        esp_err_t stop_result = esp_mqtt_client_stop(bc.client);
        if (stop_result != ESP_OK) {
          Serial.printf("MQTT reporter: broker %d stop failed (%d)\n", i + 1, (int)stop_result);
          reconcileMqttTerminal(i, now_ms);
        } else if (esp_mqtt_client_start(bc.client) != ESP_OK) {
          Serial.printf("MQTT reporter: broker %d restart failed; rebuilding client\n", i + 1);
          esp_mqtt_client_destroy(bc.client);
          bc.client = nullptr;
          bc.effective_transport = mqtt_tls_cap::Transport::Unknown;
          bc.started = false;
          bc.next_connect_attempt_ms = 0;
          bc.pending_connected_generation.store(0, std::memory_order_release);
          bc.pending_terminal_generation.store(0, std::memory_order_release);
          reconcileMqttTerminal(i, now_ms);
        }
      }
    }
  }
}

bool MqttReporter::connectMQTT(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  BrokerClient &bc = _clients[idx];
  const MqttBrokerConfig &broker = _settings.broker(idx);

  if (bc.started) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (broker.uri[0] == '\0') return false;
  if (bc.status_topic[0] == '\0' || bc.offline_payload == nullptr) return false;
  const mqtt_tls_cap::Transport requested_transport =
      mqtt_tls_cap::transportForUri(broker.uri);
  if (mqtt_tls_cap::countsAgainstTlsBudget(requested_transport) &&
      !tlsBudgetAllows(idx, requested_transport)) {
    bc.heap_inactive = true;
    return false;
  }
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
  mqtt_reconnect::onAttemptStarted(bc.recon, now);

  // A disconnect/error event leaves the old client handle allocated. Dispose of
  // it here, outside the MQTT event callback, before creating its replacement.
  if (bc.client != nullptr) {
    esp_mqtt_client_stop(bc.client);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
  }
  bc.pending_connected_generation.store(0, std::memory_order_release);
  bc.pending_terminal_generation.store(0, std::memory_order_release);
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
  // esp_mqtt_client_publish() is synchronous.  Preserve direct QoS0 throughput
  // while bounding a stalled socket call; the shared loop additionally stops
  // draining after a 250-ms/two-call budget.
  mqtt_config.network_timeout_ms = mqtt_publish::kNetworkTimeoutMs;
  // Reconnects are scheduled by the reporter loop via MqttReconnectPolicy
  // (ladder backoff + circuit breaker). esp-mqtt's own retry loop used to
  // re-hammer a down broker every few seconds with no pacing.
  mqtt_config.disable_auto_reconnect = true;
  // rev3 status payloads (full telemetry + nested mqtt block) are ~1.9 KB and
  // are also used as the LWT, so the CONNECT packet alone can exceed the
  // default 2048-byte buffers. Without this headroom esp-mqtt fails every
  // connect with "Connect message cannot be created".
  mqtt_config.buffer_size = mqtt_publish::kConnectBufferBytes;
  mqtt_config.out_buffer_size = mqtt_publish::kConnectBufferBytes;
  mqtt_config.lwt_topic = bc.status_topic;
  mqtt_config.lwt_msg = bc.offline_payload;
  mqtt_config.lwt_msg_len = (int)bc.offline_payload_len;
  mqtt_config.lwt_qos = 0;
  mqtt_config.lwt_retain = broker.retain_status != 0;
  uint32_t generation = mqtt_reconnect::nextGeneration(
      bc.client_generation.load(std::memory_order_relaxed));
  bc.client_generation.store(generation, std::memory_order_release);
  _event_ctx[idx].generation = generation;
  mqtt_config.user_context = &_event_ctx[idx];
  mqtt_config.event_handle = mqttEventHandler;
  if (strncmp(broker.uri, "wss://", 6) == 0 || strncmp(broker.uri, "mqtts://", 8) == 0) {
    mqtt_config.crt_bundle_attach = esp_crt_bundle_attach;
  }

  bc.client = esp_mqtt_client_init(&mqtt_config);
  if (bc.client == nullptr) {
    bc.connect_start_failures++;
    bc.effective_transport = mqtt_tls_cap::Transport::Unknown;
    mqtt_reconnect::onTerminalEvent(bc.recon, millis(), (uint8_t)idx);
    bc.next_connect_attempt_ms = 0;
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_init failed\n", idx + 1);
    return true;
  }
  bc.effective_transport = requested_transport;

  // Set before start so a fast disconnect/error callback cannot be overwritten
  // by a late assignment after esp_mqtt_client_start() returns.
  bc.started = true;
  if (esp_mqtt_client_start(bc.client) != ESP_OK) {
    bc.connect_start_failures++;
    mqtt_reconnect::onTerminalEvent(bc.recon, millis(), (uint8_t)idx);
    bc.next_connect_attempt_ms = 0;
    Serial.printf("MQTT reporter: broker %d esp_mqtt_client_start failed\n", idx + 1);
    esp_mqtt_client_destroy(bc.client);
    bc.client = nullptr;
    bc.pending_connected_generation.store(0, std::memory_order_release);
    bc.pending_terminal_generation.store(0, std::memory_order_release);
    bc.started = false;
    bc.effective_transport = mqtt_tls_cap::Transport::Unknown;
    return true;
  }

  Serial.printf("MQTT reporter: broker %d MQTT client started\n", idx + 1);
  return true;
}

void MqttReporter::syncTimeFromNtp() {
  // Resolve the server first: a name that does not resolve burns a full sync
  // window and points at a configuration problem, not a transient.
  IPAddress resolved;
  if (!WiFi.hostByName(MQTT_NTP_SERVER, resolved)) {
    if (_ntp_failures < UINT8_MAX) _ntp_failures++;
    if (!acceptExistingClockFallback("DNS failure")) {
      Serial.printf("MQTT reporter: NTP server '%s' did not resolve and no trustworthy clock exists; retry in %lu ms\n",
                    MQTT_NTP_SERVER, ntpRetryDelayMs(_ntp_failures));
    } else {
      Serial.printf("MQTT reporter: NTP server '%s' did not resolve; using existing clock and retrying in %lu ms\n",
                    MQTT_NTP_SERVER, ntpRetryDelayMs(_ntp_failures));
    }
    return;
  }

  // configTime() starts a background SNTP service. Stop the previous service
  // and clear its completion bit first so a late result cannot satisfy this
  // fresh attempt.
  stopNtpService();
  _ntp_attempt_generation = mqtt_reconnect::nextGeneration(_ntp_attempt_generation);
  _ntp_pending_generation = _ntp_attempt_generation;
  _ntp_sync_started_at = millis();
  _ntp_sync_pending = true;
  configTime(0, 0, MQTT_NTP_SERVER);
  Serial.printf("MQTT reporter: NTP sync started (attempt=%lu, mode=%s)\n",
                (unsigned long)_ntp_attempt_generation,
                mqtt_clock::validationModeName(MQTT_NTP_VALIDATION_MODE));
}

void MqttReporter::checkNtpSyncComplete() {
  // Never consume a completion bit unless our owner task has a current
  // attempt pending. The status API has no request identifier of its own.
  if (!_ntp_sync_pending || _ntp_pending_generation == 0) return;

  const uint32_t now_ms = (uint32_t)millis();
  const uint32_t system_epoch = timeToEpoch(time(nullptr));
  mqtt_clock::ReplyState reply = mqtt_clock::ReplyState::Reset;
  bool response_valid = false;
#ifdef MQTT_HAVE_SNTP_STATUS
  const sntp_sync_status_t sync_status = esp_sntp_get_sync_status();
  if (sync_status == SNTP_SYNC_STATUS_COMPLETED) {
    reply = mqtt_clock::ReplyState::Completed;
    response_valid = MQTT_SNTP_RESPONSE_VALIDATED != 0;
  } else if (sync_status == SNTP_SYNC_STATUS_IN_PROGRESS) {
    reply = mqtt_clock::ReplyState::InProgress;
  }
#else
  // Some older SDKs expose no status API. This is deliberately explicit:
  // epoch observation is accepted only as `ntp_completed_no_validation` and
  // is never reported as a validated exchange.
  if (mqtt_clock::plausibleEpoch(system_epoch)) {
    reply = mqtt_clock::ReplyState::Completed;
  }
#endif

  mqtt_clock::Acceptance decision = mqtt_clock::evaluateNtpAttempt(
      true,
      _ntp_pending_generation,
      _ntp_attempt_generation,
      reply,
      system_epoch,
      response_valid,
      MQTT_NTP_VALIDATION_MODE);
  if (decision.accepted) {
    recordClockAcceptance(decision, now_ms);
    return;
  }

  if ((uint32_t)(now_ms - _ntp_sync_started_at) >= MQTT_NTP_TIMEOUT_MS) {
    _ntp_sync_pending = false;
    _ntp_pending_generation = 0;
    stopNtpService();
    if (_ntp_failures < UINT8_MAX) _ntp_failures++;
    if (!acceptExistingClockFallback("timeout")) {
      Serial.printf("MQTT reporter: NTP sync timed out with no trustworthy existing clock; retry in %lu ms\n",
                    ntpRetryDelayMs(_ntp_failures));
    } else {
      Serial.printf("MQTT reporter: NTP sync timed out; keeping existing clock and retrying in %lu ms\n",
                    ntpRetryDelayMs(_ntp_failures));
    }
  }
}

void MqttReporter::stopNtpService() {
#ifdef MQTT_HAVE_SNTP_STATUS
  esp_sntp_stop();
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
#elif defined(MQTT_HAVE_SNTP_CONTROL)
  if (sntp_enabled()) sntp_stop();
#endif
}

void MqttReporter::recordClockAcceptance(
    const mqtt_clock::Acceptance &decision, uint32_t now_ms) {
  if (!decision.accepted) return;
  _clock->setCurrentTime(decision.epoch);
  _time_synced = true;
  _ntp_synced_at_ms = now_ms;
  _clock_source = decision.source;
  _ntp_sync_pending = false;
  _ntp_pending_generation = 0;
  if (mqtt_clock::isNtpSource(decision.source)) _ntp_failures = 0;
  stopNtpService();
  Serial.printf("MQTT reporter: clock accepted source=%s validated=%s epoch=%lu\n",
                mqtt_clock::sourceName(decision.source),
                decision.validated ? "yes" : "no",
                (unsigned long)decision.epoch);
}

bool MqttReporter::acceptExistingClockFallback(const char *reason) {
  mqtt_clock::Acceptance decision =
      mqtt_clock::acceptExistingClock(_clock->getCurrentTime());
  if (!decision.accepted) return false;

  // A failed refresh does not erase a previously accepted source or make its
  // age look fresh. It only leaves the current trusted clock in place.
  if (_time_synced) {
    _ntp_sync_pending = false;
    _ntp_pending_generation = 0;
    stopNtpService();
    Serial.printf("MQTT reporter: NTP %s; retaining clock source=%s age=%lu ms\n",
                  reason ? reason : "unavailable",
                  mqtt_clock::sourceName(_clock_source),
                  (unsigned long)mqtt_clock::ageMs(
                      true, _ntp_synced_at_ms, (uint32_t)millis()));
    return true;
  }

  recordClockAcceptance(decision, (uint32_t)millis());
  return true;
}

void MqttReporter::publishStatus(int idx, const char *status) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) return;
  if (bc.status_queued) return;

  size_t payload_len = 0;
  if (!buildStatusPayload(idx, status, payload_len)) {
    _build_failures++;
    return;
  }
  enqueuePublish(idx, bc.status_topic, _build_buffer, payload_len, 0,
                 _settings.broker(idx).retain_status != 0, true);
}

char *MqttReporter::allocateReporterBuffer(size_t bytes, bool prefer_psram) const {
  if (bytes == 0) return nullptr;
  void *buffer = nullptr;
  if (prefer_psram && _psram_available) {
    buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (buffer == nullptr) {
    buffer = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return static_cast<char *>(buffer);
}

void MqttReporter::releaseReporterBuffer(char *buffer) const {
  if (buffer != nullptr) heap_caps_free(buffer);
}

bool MqttReporter::replaceOfflinePayload(BrokerClient &bc,
                                         const char *payload,
                                         size_t payload_len) {
  if (payload == nullptr || payload_len > mqtt_publish::kMaxLwtPayloadBytes ||
      strlen(payload) != payload_len) {
    return false;
  }
  char *copy = allocateReporterBuffer(payload_len + 1, true);
  if (copy == nullptr) return false;
  memcpy(copy, payload, payload_len + 1);
  releaseReporterBuffer(bc.offline_payload);
  bc.offline_payload = copy;
  bc.offline_payload_len = payload_len;
  return true;
}

size_t MqttReporter::publishQueueBrokerByteCap() const {
  return mqtt_publish::brokerByteCap(_psram_available);
}

size_t MqttReporter::publishQueueTotalByteCap() const {
  return mqtt_publish::totalByteCap(_psram_available);
}

void MqttReporter::releasePublishEntry(PublishEntry &entry) {
  releaseReporterBuffer(entry.topic);
  releaseReporterBuffer(entry.payload);
  entry.topic = nullptr;
  entry.payload = nullptr;
  entry.topic_len = 0;
  entry.payload_len = 0;
  entry.copied_bytes = 0;
  entry.qos = 0;
  entry.retain = false;
  entry.is_status = false;
  entry.pending = false;
}

bool MqttReporter::enqueuePublish(
    int idx, const char *topic, const char *payload, size_t payload_len,
    uint8_t qos, bool retain, bool is_status) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS || topic == nullptr || payload == nullptr ||
      strlen(payload) != payload_len) {
    return false;
  }
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) return false;
  if (is_status && bc.status_queued) return true;

  const size_t topic_len = strlen(topic);
  const mqtt_publish::QueueAdmission admission = mqtt_publish::admitQueueEntry(
      bc.queue_count,
      MQTT_PUBLISH_QUEUE_SIZE,
      bc.queue_bytes,
      publishQueueBrokerByteCap(),
      _publish_queue_bytes_total,
      publishQueueTotalByteCap(),
      topic_len,
      payload_len);
  if (admission != mqtt_publish::QueueAdmission::Accept) {
    bc.queue_drops++;
    if (admission == mqtt_publish::QueueAdmission::BrokerByteCap ||
        admission == mqtt_publish::QueueAdmission::TotalByteCap ||
        admission == mqtt_publish::QueueAdmission::PayloadTooLarge ||
        admission == mqtt_publish::QueueAdmission::Overflow) {
      bc.queue_byte_drops++;
    }
    bc.publish_failures++;
    return false;  // Drop newest; retained entries and accounting are unchanged.
  }

  size_t copied_bytes = 0;
  if (!mqtt_publish::queueEntryBytes(topic_len, payload_len, copied_bytes)) {
    bc.queue_drops++;
    bc.queue_byte_drops++;
    bc.publish_failures++;
    return false;
  }
  char *topic_copy = allocateReporterBuffer(topic_len + 1, true);
  char *payload_copy = allocateReporterBuffer(payload_len + 1, true);
  if (topic_copy == nullptr || payload_copy == nullptr) {
    releaseReporterBuffer(topic_copy);
    releaseReporterBuffer(payload_copy);
    bc.queue_drops++;
    bc.publish_failures++;
    return false;
  }
  memcpy(topic_copy, topic, topic_len + 1);
  memcpy(payload_copy, payload, payload_len + 1);

  PublishEntry &entry = bc.publish_queue[bc.queue_tail];
  releasePublishEntry(entry);
  entry.topic = topic_copy;
  entry.payload = payload_copy;
  entry.topic_len = topic_len;
  entry.payload_len = payload_len;
  entry.copied_bytes = copied_bytes;
  entry.qos = qos;
  entry.retain = retain;
  entry.is_status = is_status;
  entry.pending = true;
  bc.queue_tail = (uint8_t)((bc.queue_tail + 1) % MQTT_PUBLISH_QUEUE_SIZE);
  bc.queue_count++;
  bc.queue_bytes += copied_bytes;
  _publish_queue_bytes_total += copied_bytes;
  if (is_status) bc.status_queued = true;
  return true;
}

void MqttReporter::drainPublishQueue(int idx, mqtt_publish::DrainState &drain) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  if (!bc.connected || bc.client == nullptr) {
    clearPublishQueue(idx);
    return;
  }

  while (bc.queue_count > 0 && mqtt_publish::mayPublish(
             drain,
             (uint32_t)(esp_timer_get_time() / 1000),
             MQTT_PUBLISHES_PER_LOOP,
             mqtt_publish::kDrainElapsedBudgetMs)) {
    PublishEntry &entry = bc.publish_queue[bc.queue_head];
    bool was_status = entry.is_status;
    int message_id = -1;
    const bool outbox_full =
        esp_mqtt_client_get_outbox_size(bc.client) >= MQTT_OUTBOX_HIGH_WATER;
    if (entry.pending && !outbox_full) {
      mqtt_publish::notePublishCall(drain);
      message_id = esp_mqtt_client_publish(
          bc.client,
          entry.topic,
          entry.payload,
          entry.payload_len,
          entry.qos,
          entry.retain);
    }

    if (outbox_full) {
      bc.outbox_drops++;
      if (bc.outbox_drops == 1 || (bc.outbox_drops % 32) == 0) {
        Serial.printf("MQTT reporter: broker %d outbox full (>= %d bytes); dropping QoS0 publishes (total %lu)\n",
                      idx + 1, MQTT_OUTBOX_HIGH_WATER, (unsigned long)bc.outbox_drops);
      }
    } else if (message_id < 0) {
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
    const size_t released_bytes = entry.copied_bytes;
    releasePublishEntry(entry);
    bc.queue_bytes = released_bytes <= bc.queue_bytes ? bc.queue_bytes - released_bytes : 0;
    _publish_queue_bytes_total = released_bytes <= _publish_queue_bytes_total
                                     ? _publish_queue_bytes_total - released_bytes
                                     : 0;
    bc.queue_head = (uint8_t)((bc.queue_head + 1) % MQTT_PUBLISH_QUEUE_SIZE);
    bc.queue_count--;
  }
}

void MqttReporter::clearPublishQueue(int idx) {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[idx];
  while (bc.queue_count > 0) {
    PublishEntry &entry = bc.publish_queue[bc.queue_head];
    const size_t released_bytes = entry.copied_bytes;
    releasePublishEntry(entry);
    bc.queue_bytes = released_bytes <= bc.queue_bytes ? bc.queue_bytes - released_bytes : 0;
    _publish_queue_bytes_total = released_bytes <= _publish_queue_bytes_total
                                     ? _publish_queue_bytes_total - released_bytes
                                     : 0;
    bc.queue_head = (uint8_t)((bc.queue_head + 1) % MQTT_PUBLISH_QUEUE_SIZE);
    bc.queue_count--;
  }
  bc.queue_head = 0;
  bc.queue_tail = 0;
  bc.queue_bytes = 0;
  bc.status_queued = false;
}

bool MqttReporter::anyBrokerConnected() const {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    if (_clients[i].connected && _clients[i].client != nullptr) return true;
  }
  return false;
}

void MqttReporter::enqueueCallbackEvent(const CallbackEvent &event) {
  if (_callback_queue != nullptr && xQueueSend(_callback_queue, &event, 0) == pdTRUE) {
    if (event.type == CallbackEventType::WifiDisconnected ||
        event.type == CallbackEventType::WifiGotIp) {
      _pending_wifi_event.store(0, std::memory_order_release);
    } else if (event.broker_idx >= 0 && event.broker_idx < MQTT_MAX_BROKERS) {
      BrokerClient &bc = _clients[event.broker_idx];
      uint32_t active_generation = bc.client_generation.load(std::memory_order_acquire);
      if (mqtt_reconnect::generationMatches(active_generation, event.generation)) {
        bc.pending_connected_generation.store(0, std::memory_order_release);
        bc.pending_terminal_generation.store(0, std::memory_order_release);
        bc.pending_terminal_type.store(0, std::memory_order_release);
      }
    }
    return;
  }

  bool is_mqtt_event = event.type == CallbackEventType::MqttConnected ||
                       event.type == CallbackEventType::MqttDisconnected ||
                       event.type == CallbackEventType::MqttError;
  if (is_mqtt_event && event.broker_idx >= 0 && event.broker_idx < MQTT_MAX_BROKERS) {
    BrokerClient &bc = _clients[event.broker_idx];
    bc.callback_queue_drops.fetch_add(1, std::memory_order_relaxed);
    uint32_t active_generation = bc.client_generation.load(std::memory_order_acquire);
    if (!mqtt_reconnect::generationMatches(active_generation, event.generation)) {
      return;
    }
    if (event.type == CallbackEventType::MqttConnected) {
      bc.pending_terminal_generation.store(0, std::memory_order_release);
      bc.pending_terminal_type.store(0, std::memory_order_release);
      bc.pending_connected_generation.store(event.generation, std::memory_order_release);
    } else if ((event.type == CallbackEventType::MqttDisconnected ||
                event.type == CallbackEventType::MqttError) &&
               event.generation != 0) {
      // The owner loop consumes this one-slot fallback even when the queue
      // could not be allocated. Multiple terminal callbacks may collapse,
      // which is safe because their policy transition is idempotent.
      bc.pending_connected_generation.store(0, std::memory_order_release);
      bc.pending_terminal_type.store((uint8_t)event.type, std::memory_order_relaxed);
      bc.pending_error_type.store(event.error_type, std::memory_order_relaxed);
      bc.pending_error_code.store(event.error_code, std::memory_order_relaxed);
      bc.pending_tls_last_error.store(event.tls_last_error, std::memory_order_relaxed);
      bc.pending_tls_stack_error.store(event.tls_stack_error, std::memory_order_relaxed);
      bc.pending_tls_cert_verify_flags.store(event.tls_cert_verify_flags, std::memory_order_relaxed);
      bc.pending_transport_sock_errno.store(event.transport_sock_errno, std::memory_order_relaxed);
      bc.pending_connect_return_code.store(event.connect_return_code, std::memory_order_relaxed);
      bc.pending_terminal_generation.store(event.generation, std::memory_order_release);
    }
  } else {
    _wifi_callback_queue_drops.fetch_add(1, std::memory_order_relaxed);
    if (event.type == CallbackEventType::WifiDisconnected) {
      _pending_wifi_disconnect_reason.store(event.wifi_reason, std::memory_order_relaxed);
    }
    _pending_wifi_event.store((uint8_t)event.type + 1, std::memory_order_release);
  }
}

void MqttReporter::processCallbackEvents() {
  if (_callback_queue != nullptr) {
    CallbackEvent event;
    while (xQueueReceive(_callback_queue, &event, 0) == pdTRUE) {
      if (event.type == CallbackEventType::WifiDisconnected ||
          event.type == CallbackEventType::WifiGotIp) {
        handleWifiEvent(event);
        continue;
      }
      handleMqttEvent(event);
    }
  }

  uint8_t pending_wifi = _pending_wifi_event.exchange(0, std::memory_order_acq_rel);
  if (pending_wifi != 0) {
    CallbackEvent event = {};
    event.broker_idx = -1;
    event.type = static_cast<CallbackEventType>(pending_wifi - 1);
    event.wifi_reason = _pending_wifi_disconnect_reason.load(std::memory_order_relaxed);
    handleWifiEvent(event);
  }
  processPendingTerminalEvents();
}

void MqttReporter::handleWifiEvent(const CallbackEvent &event) {
  uint32_t now_ms = (uint32_t)millis();
  if (event.type == CallbackEventType::WifiDisconnected) {
    _last_wifi_disconnect_reason = event.wifi_reason;
    _wifi_got_ip = false;
    _wifi_connected_since_ms = 0;
    _wifi_last_ip = "";
    for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
      reconcileMqttTerminal(i, now_ms);
    }
    Serial.printf("MQTT reporter: WiFi disconnected, reason=%d\n",
                  _last_wifi_disconnect_reason);
    return;
  }
  if (event.type != CallbackEventType::WifiGotIp) return;

  _wifi_got_ip = true;
  _wifi_connected_since_ms = now_ms;
  _wifi_last_ip = WiFi.localIP().toString();
  Serial.printf("MQTT reporter: WiFi got IP: %s\n", _wifi_last_ip.c_str());
  // The failure domain changed (network is back): keep cap/breaker
  // accounting, but replace an obsolete outage deadline with a prompt,
  // staggered probe.
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    const MqttBrokerConfig &broker = _settings.broker(i);
    if (broker.enabled && broker.uri[0] != '\0' && !_clients[i].connected) {
      mqtt_reconnect::onWifiRecovered(_clients[i].recon, now_ms, (uint8_t)i);
      _clients[i].recon_seeded = true;
    }
  }
}

void MqttReporter::processPendingTerminalEvents() {
  for (int i = 0; i < MQTT_MAX_BROKERS; i++) {
    BrokerClient &bc = _clients[i];
    uint32_t connected_generation =
        bc.pending_connected_generation.exchange(0, std::memory_order_acq_rel);
    uint32_t active_generation = bc.client_generation.load(std::memory_order_acquire);
    if (mqtt_reconnect::generationMatches(active_generation, connected_generation) &&
        bc.client != nullptr) {
      CallbackEvent connected = {};
      connected.type = CallbackEventType::MqttConnected;
      connected.broker_idx = (int8_t)i;
      connected.client = bc.client;
      connected.generation = connected_generation;
      handleMqttEvent(connected);
    }

    uint32_t generation = bc.pending_terminal_generation.exchange(0, std::memory_order_acq_rel);
    if (!mqtt_reconnect::generationMatches(active_generation, generation) ||
        bc.client == nullptr) {
      continue;
    }
    Serial.printf("MQTT reporter: broker %d reconciling dropped terminal callback\n", i + 1);
    CallbackEvent terminal = {};
    terminal.type = static_cast<CallbackEventType>(bc.pending_terminal_type.load(std::memory_order_relaxed));
    if (terminal.type != CallbackEventType::MqttDisconnected &&
        terminal.type != CallbackEventType::MqttError) {
      terminal.type = CallbackEventType::MqttDisconnected;
    }
    terminal.broker_idx = (int8_t)i;
    terminal.client = bc.client;
    terminal.generation = generation;
    terminal.error_type = bc.pending_error_type.load(std::memory_order_relaxed);
    terminal.error_code = bc.pending_error_code.load(std::memory_order_relaxed);
    terminal.tls_last_error = bc.pending_tls_last_error.load(std::memory_order_relaxed);
    terminal.tls_stack_error = bc.pending_tls_stack_error.load(std::memory_order_relaxed);
    terminal.tls_cert_verify_flags = bc.pending_tls_cert_verify_flags.load(std::memory_order_relaxed);
    terminal.transport_sock_errno = bc.pending_transport_sock_errno.load(std::memory_order_relaxed);
    terminal.connect_return_code = bc.pending_connect_return_code.load(std::memory_order_relaxed);
    bc.pending_terminal_type.store(0, std::memory_order_release);
    handleMqttEvent(terminal);
  }
}

bool MqttReporter::reconcileMqttTerminal(int broker_idx, uint32_t now_ms) {
  if (broker_idx < 0 || broker_idx >= MQTT_MAX_BROKERS) return false;
  BrokerClient &bc = _clients[broker_idx];
  bc.connected = false;
  bc.connected_since_ms = 0;
  bc.started = bc.client != nullptr;
  bc.online_status_pending = false;
  bool advanced = mqtt_reconnect::onTerminalEvent(bc.recon, now_ms, (uint8_t)broker_idx);
  if (advanced) {
    Serial.printf("MQTT reporter: broker %d retry in %lu ms (rung %u%s)\n",
                  broker_idx + 1,
                  (unsigned long)mqtt_reconnect::nextWaitMs(bc.recon, now_ms),
                  (unsigned int)bc.recon.rung,
                  bc.recon.breaker ? ", breaker open" : "");
  }
  return advanced;
}

void MqttReporter::handleMqttEvent(const CallbackEvent &event) {
  if (event.broker_idx < 0 || event.broker_idx >= MQTT_MAX_BROKERS) return;
  BrokerClient &bc = _clients[event.broker_idx];
  if (event.client == nullptr || event.client != bc.client ||
      !mqtt_reconnect::generationMatches(
          bc.client_generation.load(std::memory_order_acquire), event.generation)) {
    return;
  }

  switch (event.type) {
    case CallbackEventType::MqttConnected:
      if (!mqtt_reconnect::onConnected(bc.recon, (uint32_t)millis())) break;
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
      // esp-mqtt auto-reconnect is disabled; the reporter loop retries on the
      // MqttReconnectPolicy schedule. Keep the client handle alive and count
      // exactly one ladder step per failed attempt / dropped session.
      bc.disconnect_events++;
      reconcileMqttTerminal(event.broker_idx, (uint32_t)millis());
      break;
    case CallbackEventType::MqttError:
      Serial.printf("MQTT reporter: broker %d error type=%d code=%d\n", event.broker_idx + 1,
                    event.error_type, event.error_code);
      bc.last_error_type = event.error_type;
      bc.last_error_code = event.error_code;
      bc.last_tls_last_error = event.tls_last_error;
      bc.last_tls_stack_error = event.tls_stack_error;
      bc.last_tls_cert_verify_flags = event.tls_cert_verify_flags;
      bc.last_transport_sock_errno = event.transport_sock_errno;
      bc.last_connect_return_code = event.connect_return_code;
      bc.last_error_at_ms = (uint32_t)millis();
      bc.error_events++;
      reconcileMqttTerminal(event.broker_idx, (uint32_t)millis());
      break;
    default:
      break;
  }
}

bool MqttReporter::brokerNeedsTimeSync(int idx) const {
  if (idx < 0 || idx >= MQTT_MAX_BROKERS) return false;
  return mqtt_tls_cap::countsAgainstTlsBudget(
      mqtt_tls_cap::transportForUri(_settings.broker(idx).uri));
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

bool MqttReporter::buildNeighborsPayload(uint32_t now_ms, size_t &payload_len) const {
  payload_len = 0;
  mqtt_publish::CheckedBufferBuilder builder(
      _build_buffer, mqtt_publish::kBuildBufferBytes);
  builder.append("{\"nodes\":[");
  bool first = true;
  for (int i = 0; i < NEIGHBOR_CAPACITY; i++) {
    const NeighborEntry &entry = _neighbors[i];
    if (!entry.used) continue;
    if (!first) builder.append(",");
    first = false;
    builder.append("{\"id\":\"");
    builder.appendHex(entry.id, PUB_KEY_SIZE);
    builder.append("\",\"rssi\":");
    builder.appendSigned(entry.rssi);
    builder.append(",\"snr\":");
    builder.appendFloat1(entry.snr);
    builder.append(",\"last_seen_s\":");
    builder.appendUnsigned((uint32_t)(now_ms - entry.last_heard_ms) / 1000UL);
    builder.append("}");
  }
  builder.append("]}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxPublishPayloadBytes) return false;
  payload_len = builder.length();
  return true;
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

    size_t payload_len = 0;
    if (!buildNeighborsPayload(now_ms, payload_len)) {
      _build_failures++;
      continue;
    }
    if (enqueuePublish(i, bc.neighbors_topic, _build_buffer, payload_len,
                       0, false, false)) {
      bc.last_neighbors_publish = now_ms;
    }
  }
}

void MqttReporter::appendCpuIdleStats(mqtt_publish::CheckedBufferBuilder &builder) const {
  if (_idle_pct_core0 < 0.0f) {
    builder.append(",\"idle_pct_core0\":null");
  } else {
    builder.append(",\"idle_pct_core0\":");
    builder.appendFloat1(_idle_pct_core0);
  }
#if portNUM_PROCESSORS > 1
  if (_idle_pct_core1 < 0.0f) {
    builder.append(",\"idle_pct_core1\":null");
  } else {
    builder.append(",\"idle_pct_core1\":");
    builder.appendFloat1(_idle_pct_core1);
  }
#else
  builder.append(",\"idle_pct_core1\":0.0");
#endif
}

bool MqttReporter::appendStatusStatsPayload(
    mqtt_publish::CheckedBufferBuilder &builder, int broker_idx) const {
  const uint32_t now_ms = millis();
  const bool wifi_connected = isWiFiConnected();
  const auto appendUnsignedField = [&builder](const char *key, uint64_t value) {
    builder.append(key);
    builder.appendUnsigned(value);
  };
  const auto appendSignedField = [&builder](const char *key, int64_t value) {
    builder.append(key);
    builder.appendSigned(value);
  };

  builder.append("{");
  appendUnsignedField("\"uptime_ms\":", now_ms);
  appendUnsignedField(",\"boot_count\":", _boot_count);
  builder.append(",\"reset_reason\":\"");
  builder.appendJsonEscaped(_reset_reason);
  builder.append("\"");
  builder.append(",\"ntp_synced\":");
  builder.append(mqtt_clock::isNtpSource(_clock_source) ? "true" : "false");
  builder.append(",\"clock_trusted\":");
  builder.append(_time_synced ? "true" : "false");
  builder.append(",\"ntp_sync_source\":\"");
  builder.append(mqtt_clock::sourceName(_clock_source));
  builder.append("\"");
  builder.append(",\"ntp_sync_validated\":");
  builder.append(mqtt_clock::isValidated(_clock_source) ? "true" : "false");
  builder.append(",\"ntp_sync_fallback\":");
  builder.append(mqtt_clock::isFallback(_clock_source) ? "true" : "false");
  builder.append(",\"ntp_validation_mode\":\"");
  builder.append(mqtt_clock::validationModeName(MQTT_NTP_VALIDATION_MODE));
  builder.append("\"");
  appendUnsignedField(",\"ntp_sync_age_ms\":",
                      mqtt_clock::ageMs(_time_synced, (uint32_t)_ntp_synced_at_ms, now_ms));
  appendUnsignedField(",\"ntp_attempt_generation\":", _ntp_attempt_generation);
  appendUnsignedField(",\"ntp_last_attempt_age_ms\":",
                      mqtt_clock::ageMs(_ntp_attempted, (uint32_t)_last_ntp_attempt, now_ms));
  builder.append(",\"boot_epoch\":");
  time_t current_epoch = time(nullptr);
  uint32_t boot_epoch = 0;
  uint32_t uptime_secs = now_ms / 1000UL;
  if (_time_synced && current_epoch >= MQTT_VALID_EPOCH && current_epoch >= (time_t)uptime_secs) {
    boot_epoch = (uint32_t)(current_epoch - (time_t)uptime_secs);
  }
  builder.appendUnsigned(boot_epoch);
  appendUnsignedField(",\"max_loop_ms\":", _max_loop_ms);
  appendUnsignedField(",\"max_loop_at_ms\":", _max_loop_at_ms);
  appendUnsignedField(",\"loop_iterations\":", _loop_iterations);
  appendUnsignedField(",\"wifi_reconnect_attempts\":", _wifi_reconnect_attempts);
  appendUnsignedField(",\"rx_publish_calls\":", _rx_publish_calls);
  appendUnsignedField(",\"tx_publish_calls\":", _tx_publish_calls);
  appendUnsignedField(",\"tx_fail_publish_calls\":", _tx_fail_publish_calls);
  appendUnsignedField(",\"publish_skipped_no_connection\":", _publish_skipped_no_connection);
  appendUnsignedField(",\"build_failures\":", _build_failures);
  appendUnsignedField(",\"serialize_preflight_drops\":", _serialize_preflight_drops);
  appendUnsignedField(",\"forward_successes\":", _mesh->getForwardSuccessCount());
  appendUnsignedField(",\"forward_successes_flood\":", _mesh->getForwardFloodSuccessCount());
  appendUnsignedField(",\"forward_successes_direct\":", _mesh->getForwardDirectSuccessCount());
  appendUnsignedField(",\"forward_failures\":", _mesh->getForwardFailureCount());
  appendUnsignedField(",\"tx_queue_depth\":", _mesh->getTxQueueDepth());
  appendUnsignedField(",\"tx_queue_depth_peak\":", _mesh->getTxQueuePeakDepth());
  appendUnsignedField(",\"heap_free\":", ESP.getFreeHeap());
  appendUnsignedField(",\"heap_min_free\":", ESP.getMinFreeHeap());
  appendUnsignedField(",\"heap_min_seen_since_boot\":", _min_free_heap);
  appendUnsignedField(",\"heap_internal_free\":",
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  appendUnsignedField(",\"heap_internal_largest_block\":",
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  appendUnsignedField(",\"heap_psram_free\":",
                      _psram_available ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : 0);
  appendUnsignedField(",\"heap_psram_largest_block\":",
                      _psram_available ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : 0);
  builder.append(",\"wifi_connected\":");
  builder.append(wifi_connected ? "true" : "false");
  appendUnsignedField(",\"wifi_uptime_ms\":",
                      _wifi_connected_since_ms != 0 ? now_ms - _wifi_connected_since_ms : 0);
  builder.append(",\"wifi_rssi\":");
  if (wifi_connected) {
    builder.appendSigned(WiFi.RSSI());
  } else {
    builder.append("null");
  }
  String wifi_ssid = WiFi.SSID();
  if (wifi_connected && wifi_ssid.length() == 0) return false;
  builder.append(",\"wifi_ssid\":\"");
  builder.appendJsonEscaped(wifi_ssid.c_str());
  builder.append("\"");
  appendUnsignedField(",\"nodes_heard_24h\":", heardNodesLast24Hours(now_ms));
  appendSignedField(",\"last_rx_rssi\":", _last_rx_rssi);
  builder.append(",\"last_rx_snr\":");
  builder.appendFloat1(_last_rx_snr);
  appendSignedField(",\"tx_power_dbm\":", _mesh->getTxPowerDbm());
  appendSignedField(",\"last_tx_fail_reason\":", _last_tx_fail_reason);
  appendUnsignedField(",\"config_version\":", MqttSettingsStore::configVersion());
  char config_crc[12];
  snprintf(config_crc, sizeof(config_crc), "0x%08lX", (unsigned long)_config_crc32);
  builder.append(",\"config_crc32\":\"");
  builder.append(config_crc);
  builder.append("\"");
  builder.append(",\"fs_free_bytes\":");
  const size_t fs_total_bytes = SPIFFS.totalBytes();
  const size_t fs_used_bytes = SPIFFS.usedBytes();
  builder.appendUnsigned(fs_total_bytes >= fs_used_bytes ? fs_total_bytes - fs_used_bytes : 0);
  appendUnsignedField(",\"fs_total_bytes\":", fs_total_bytes);
  nvs_stats_t nvs_stats = {};
  if (nvs_get_stats(nullptr, &nvs_stats) == ESP_OK) {
    appendUnsignedField(",\"nvs_free_entries\":", nvs_stats.free_entries);
  } else {
    builder.append(",\"nvs_free_entries\":null");
  }
  builder.append(",\"power_source\":\"");
  builder.append(board.isExternalPowered() ? "usb" : "battery");
  builder.append("\"");
  builder.append(",\"solar_mv\":null");
  float board_temp_c = readBoardTemperatureC();
  if (isfinite(board_temp_c)) {
    builder.append(",\"board_temp_c\":");
    builder.appendFloat1(board_temp_c);
  } else {
    builder.append(",\"board_temp_c\":null");
  }
  String channel_id = channelKeyId();
  if (channel_id.length() >= 8) {
    char short_channel_id[9];
    memcpy(short_channel_id, channel_id.c_str(), 8);
    short_channel_id[8] = '\0';
    builder.append(",\"channel_id\":\"");
    builder.appendJsonEscaped(short_channel_id);
    builder.append("\"");
  } else {
    builder.append(",\"channel_id\":null");
  }
  builder.append(",\"git_commit\":\"");
  builder.appendJsonEscaped(MESHCORE_GIT_COMMIT);
  builder.append("\"");
  appendCpuIdleStats(builder);

  // Keep battery and airtime/utilization values sourced from the same core
  // stats path used by the CLI, rather than duplicating board/radio reads here.
  String mesh_stats = _mesh->buildMqttStatusStatsJson();
  static const char *const required_mesh_fields[] = {
      "\"battery_mv\":", "\"uptime_secs\":", "\"tx_air_secs\":",
      "\"rx_air_secs\":", "\"channel_utilization\":", "\"air_util_tx\":",
      "\"air_util_rx\":"};
  if (mesh_stats.length() < 2 || mesh_stats[0] != '{' ||
      mesh_stats[mesh_stats.length() - 1] != '}') return false;
  for (const char *required : required_mesh_fields) {
    if (mesh_stats.indexOf(required) < 0) return false;
  }
  if (mesh_stats.length() > 2) {
    builder.append(",");
    builder.append(mesh_stats.c_str() + 1, mesh_stats.length() - 2);
  }

  if (broker_idx >= 0 && broker_idx < MQTT_MAX_BROKERS) {
    const BrokerClient &bc = _clients[broker_idx];
    const MqttBrokerConfig &broker = _settings.broker(broker_idx);
    builder.append(",\"mqtt\":{");
    appendUnsignedField("\"broker_index\":", broker_idx + 1);
    appendUnsignedField(",\"connect_attempts\":", bc.connect_attempts);
    appendUnsignedField(",\"connect_start_failures\":", bc.connect_start_failures);
    appendUnsignedField(",\"connect_events\":", bc.connect_events);
    appendUnsignedField(",\"disconnect_events\":", bc.disconnect_events);
    appendUnsignedField(",\"error_events\":", bc.error_events);
    appendUnsignedField(",\"reconnect_rung\":", bc.recon.rung);
    builder.append(",\"reconnect_breaker\":");
    builder.append(bc.recon.breaker ? "true" : "false");
    appendUnsignedField(",\"reconnect_next_in_ms\":",
                        mqtt_reconnect::nextWaitMs(bc.recon, now_ms));
    appendSignedField(",\"last_error_type\":", bc.last_error_type);
    appendSignedField(",\"last_error_code\":", bc.last_error_code);
    appendSignedField(",\"last_tls_err\":", bc.last_tls_last_error);
    appendSignedField(",\"last_tls_stack_err\":", bc.last_tls_stack_error);
    appendSignedField(",\"last_tls_cert_verify_flags\":", bc.last_tls_cert_verify_flags);
    appendSignedField(",\"last_sock_errno\":", bc.last_transport_sock_errno);
    appendSignedField(",\"last_connect_return_code\":", bc.last_connect_return_code);
    appendUnsignedField(",\"last_error_at_ms\":", bc.last_error_at_ms);
    appendUnsignedField(",\"last_error_age_ms\":",
                        mqtt_clock::ageMs(bc.last_error_at_ms != 0, bc.last_error_at_ms, now_ms));
    builder.append(",\"heap_inactive\":");
    builder.append(bc.heap_inactive ? "true" : "false");
    builder.append(",\"effective_transport\":\"");
    builder.append(mqtt_tls_cap::transportName(bc.effective_transport));
    builder.append("\"");
    appendUnsignedField(",\"tls_live\":", liveTlsCount());
    appendUnsignedField(",\"tls_cap\":", tlsCap());
    String broker_uri = sanitizeBrokerUri(broker.uri);
    if (broker.uri[0] != '\0' && broker_uri.length() == 0) return false;
    builder.append(",\"broker_uri\":\"");
    builder.appendJsonEscaped(broker_uri.c_str());
    builder.append("\",\"broker_username\":\"");
    builder.appendJsonEscaped(broker.username);
    builder.append("\"");
    appendUnsignedField(",\"reconnect_attempts_1h\":", reconnectAttemptsLastHour(bc, now_ms));
    appendUnsignedField(",\"status_publishes\":", bc.status_publish_count);
    appendUnsignedField(",\"packet_publishes\":", bc.packet_publish_count);
    appendUnsignedField(",\"session_status_publishes\":", bc.session_status_publish_count);
    appendUnsignedField(",\"session_packet_publishes\":", bc.session_packet_publish_count);
    appendUnsignedField(",\"publish_failures\":", bc.publish_failures);
    appendUnsignedField(",\"publish_queue_depth\":", bc.queue_count);
    appendUnsignedField(",\"publish_queue_cap\":", MQTT_PUBLISH_QUEUE_SIZE);
    appendUnsignedField(",\"publish_queue_drops\":", bc.queue_drops);
    appendUnsignedField(",\"publish_queue_byte_drops\":", bc.queue_byte_drops);
    appendUnsignedField(",\"publish_queue_bytes\":", bc.queue_bytes);
    appendUnsignedField(",\"publish_queue_byte_cap\":", publishQueueBrokerByteCap());
    appendUnsignedField(",\"publish_queue_total_bytes\":", _publish_queue_bytes_total);
    appendUnsignedField(",\"publish_queue_total_byte_cap\":", publishQueueTotalByteCap());
    appendSignedField(",\"publish_outbox_size\":",
                      bc.client != nullptr ? esp_mqtt_client_get_outbox_size(bc.client) : -1);
    appendUnsignedField(",\"publish_outbox_cap\":", MQTT_OUTBOX_HIGH_WATER);
    appendUnsignedField(",\"publish_outbox_drops\":", bc.outbox_drops);
    builder.append(",\"connected\":");
    builder.append(bc.connected ? "true" : "false");
    appendUnsignedField(",\"uptime_ms\":",
                        bc.connected_since_ms != 0 ? now_ms - bc.connected_since_ms : 0);
    appendUnsignedField(",\"neighbor_interval_s\":", broker.neighbor_interval_secs);
    appendUnsignedField(",\"last_offline_epoch\":", bc.last_offline_epoch);
    builder.append("}");
  }

  builder.append("}");
  return builder.ok();
}

bool MqttReporter::buildStatusPayload(
    int broker_idx, const char *status, size_t &payload_len) const {
  payload_len = 0;
  const MqttSharedConfig &shared = _settings.shared();
  mqtt_publish::CheckedBufferBuilder builder(
      _build_buffer, mqtt_publish::kMaxLwtPayloadBytes + 1);
  builder.append("{");
  if (status != nullptr && status[0] != '\0') {
    builder.append("\"status\":\"");
    builder.appendJsonEscaped(status);
    builder.append("\",");
  }
  builder.append("\"origin\":\"");
  builder.appendJsonEscaped(_mesh->getNodeName());
  builder.append("\",\"origin_id\":\"");
  builder.append(_origin_id);
  builder.append("\",\"model\":\"");
  builder.appendJsonEscaped(shared.model);
  builder.append("\",\"firmware_version\":\"");
  builder.appendJsonEscaped(FIRMWARE_VERSION);
  builder.append("\",\"radio\":\"");
  char radio_buf[64];
  const NodePrefs *prefs = _mesh->getNodePrefs();
  const float freq = prefs ? prefs->freq : LORA_FREQ;
  const float bw = prefs ? prefs->bw : LORA_BW;
  const int sf = prefs ? prefs->sf : LORA_SF;
  const int cr = prefs ? prefs->cr : LORA_CR;
  int radio_len = snprintf(radio_buf, sizeof(radio_buf), "SX1262 %.3f/%.0f/%d/%d",
                           (double)freq, (double)bw, sf, cr);
  if (radio_len <= 0 || (size_t)radio_len >= sizeof(radio_buf)) return false;
  builder.appendJsonEscaped(radio_buf);
  builder.append("\",\"client_version\":\"");
  builder.appendJsonEscaped(shared.client_version);
  builder.append("\"");
  if (_time_synced) {
    DateTime dt(_clock->getCurrentTime());
    char timestamp[40];
    int timestamp_len = snprintf(timestamp, sizeof(timestamp),
                                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                 dt.year(), dt.month(), dt.day(),
                                 dt.hour(), dt.minute(), dt.second());
    if (timestamp_len <= 0 || (size_t)timestamp_len >= sizeof(timestamp)) return false;
    builder.append(",\"timestamp\":\"");
    builder.append(timestamp);
    builder.append("\"");
  } else {
    builder.append(",\"timestamp\":null");
  }
  builder.append(",\"stats\":");
  if (!appendStatusStatsPayload(builder, broker_idx)) return false;
  builder.append("}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxLwtPayloadBytes) return false;
  payload_len = builder.length();
  return true;
}

bool MqttReporter::buildPacketPayload(
    const char *direction,
    mesh::Packet *pkt,
    int len,
    const uint8_t *raw,
    size_t raw_len,
    float score,
    int rssi,
    float snr,
    uint32_t duration_ms,
    bool include_radio_metrics,
    size_t &payload_len) const {
  payload_len = 0;
  if (direction == nullptr || pkt == nullptr || raw == nullptr || raw_len > MAX_TRANS_UNIT) return false;
  const mqtt_wire::Preflight preflight = mqtt_wire::validate(
      pkt->payload_len, pkt->path_len, pkt->hasTransportCodes(),
      MAX_PACKET_PAYLOAD, MAX_PATH_SIZE, MAX_TRANS_UNIT);
  if (!preflight.ok()) return false;

  uint8_t packet_hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(packet_hash);
  mqtt_publish::CheckedBufferBuilder builder(
      _build_buffer, mqtt_publish::kBuildBufferBytes);
  builder.append("{\"origin\":\"");
  builder.appendJsonEscaped(_mesh->getNodeName());
  builder.append("\",\"origin_id\":\"");
  builder.append(_origin_id);
  builder.append("\"");
  DateTime dt(_clock->getCurrentTime());
  if (_time_synced) {
    char timestamp[40];
    int timestamp_len = snprintf(timestamp, sizeof(timestamp),
                                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                 dt.year(), dt.month(), dt.day(),
                                 dt.hour(), dt.minute(), dt.second());
    if (timestamp_len <= 0 || (size_t)timestamp_len >= sizeof(timestamp)) return false;
    builder.append(",\"timestamp\":\"");
    builder.append(timestamp);
    builder.append("\"");
  } else {
    builder.append(",\"timestamp\":null");
  }
  builder.append(",\"type\":\"PACKET\",\"direction\":\"");
  builder.appendJsonEscaped(direction);
  builder.append("\"");
  if (_time_synced) {
    char time_field[16];
    char date_field[16];
    int time_len = snprintf(time_field, sizeof(time_field), "%02d:%02d:%02d",
                            dt.hour(), dt.minute(), dt.second());
    int date_len = snprintf(date_field, sizeof(date_field), "%d/%d/%d",
                            dt.day(), dt.month(), dt.year());
    if (time_len <= 0 || (size_t)time_len >= sizeof(time_field) ||
        date_len <= 0 || (size_t)date_len >= sizeof(date_field)) return false;
    builder.append(",\"time\":\"");
    builder.append(time_field);
    builder.append("\",\"date\":\"");
    builder.append(date_field);
    builder.append("\"");
  } else {
    builder.append(",\"time\":null,\"date\":null");
  }
  builder.append(",\"len\":\"");
  builder.appendSigned(len);
  builder.append("\",\"packet_type\":\"");
  builder.appendUnsigned(pkt->getPayloadType());
  builder.append("\",\"route\":\"");
  builder.append(pkt->isRouteDirect() ? "D" : "F");
  builder.append("\",\"payload_len\":\"");
  builder.appendUnsigned(pkt->payload_len);
  builder.append("\",\"raw\":\"");
  builder.appendHex(raw, raw_len);
  builder.append("\"");

  if (include_radio_metrics) {
    builder.append(",\"SNR\":\"");
    builder.appendSigned((int)snr);
    builder.append("\",\"RSSI\":\"");
    builder.appendSigned(rssi);
    builder.append("\",\"score\":\"");
    builder.appendSigned((int)(score * 1000.0f));
    builder.append("\",\"duration\":\"");
    builder.appendUnsigned(duration_ms);
    builder.append("\"");
  }

  builder.append(",\"hash\":\"");
  builder.appendHex(packet_hash, MAX_HASH_SIZE);
  builder.append("\"");

  if (pkt->isRouteDirect() && shouldIncludePath(pkt)) {
    char path_buf[16];
    int path_len = snprintf(path_buf, sizeof(path_buf), "%02X -> %02X",
                            (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path_buf)) return false;
    builder.append(",\"path\":\"");
    builder.append(path_buf);
    builder.append("\"");
  }

  builder.append("}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxPublishPayloadBytes) return false;
  payload_len = builder.length();
  return true;
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
  int changed_broker_idx = -1;
  const bool restart_live_client =
      reporterConfigNeedsClientRestart(key, &changed_broker_idx);
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
  if (restart_live_client && changed_broker_idx >= 0 &&
      changed_broker_idx < MQTT_MAX_BROKERS &&
      _clients[changed_broker_idx].client != nullptr) {
    Serial.printf("MQTT reporter: broker %d connection settings changed; applying via owner-task teardown\n",
                  changed_broker_idx + 1);
    resetBrokerConnection(changed_broker_idx);
  }
  return true;
}

bool MqttReporter::resetConfig() {
  _settings.resetToDefaults();
  _settings.clearWriteHold();
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
  snprintf(line, sizeof(line), "    callback.queue_drops=%lu",
           (unsigned long)bc.callback_queue_drops.load(std::memory_order_relaxed));
  out.println(line);
  snprintf(line, sizeof(line), "    reconnect.rung=%u", (unsigned int)bc.recon.rung);
  out.println(line);
  snprintf(line, sizeof(line), "    reconnect.breaker=%s", bc.recon.breaker ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "    reconnect.next_in_ms=%lu",
           (unsigned long)mqtt_reconnect::nextWaitMs(bc.recon, (uint32_t)millis()));
  out.println(line);
  snprintf(line, sizeof(line), "    status.publishes=%lu", (unsigned long)bc.status_publish_count);
  out.println(line);
  snprintf(line, sizeof(line), "    packet.publishes=%lu", (unsigned long)bc.packet_publish_count);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.failures=%lu", (unsigned long)bc.publish_failures);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_depth=%u/%u",
           (unsigned int)bc.queue_count, (unsigned int)MQTT_PUBLISH_QUEUE_SIZE);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_drops=%lu", (unsigned long)bc.queue_drops);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_byte_drops=%lu",
           (unsigned long)bc.queue_byte_drops);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.queue_bytes=%u/%u total=%u/%u",
           (unsigned int)bc.queue_bytes,
           (unsigned int)publishQueueBrokerByteCap(),
           (unsigned int)_publish_queue_bytes_total,
           (unsigned int)publishQueueTotalByteCap());
  out.println(line);
  snprintf(line, sizeof(line), "    publish.outbox_size=%d/%d",
           bc.client != nullptr ? esp_mqtt_client_get_outbox_size(bc.client) : -1,
           MQTT_OUTBOX_HIGH_WATER);
  out.println(line);
  snprintf(line, sizeof(line), "    publish.outbox_drops=%lu", (unsigned long)bc.outbox_drops);
  out.println(line);
  snprintf(line, sizeof(line), "    last_error.type=%d code=%d at_ms=%lu age_ms=%lu",
           bc.last_error_type, bc.last_error_code,
           (unsigned long)bc.last_error_at_ms,
           (unsigned long)mqtt_clock::ageMs(
               bc.last_error_at_ms != 0, bc.last_error_at_ms, (uint32_t)millis()));
  out.println(line);
  snprintf(line, sizeof(line), "    last_error.tls=%d stack=%d cert_flags=%d sock=%d mqtt_rc=%d",
           bc.last_tls_last_error, bc.last_tls_stack_error,
           bc.last_tls_cert_verify_flags, bc.last_transport_sock_errno,
           bc.last_connect_return_code);
  out.println(line);
  snprintf(line, sizeof(line), "    effective_transport=%s tls_live=%u/%u",
           mqtt_tls_cap::transportName(bc.effective_transport),
           (unsigned int)liveTlsCount(), (unsigned int)tlsCap());
  out.println(line);
  snprintf(line, sizeof(line), "    heap.inactive=%s", bc.heap_inactive ? "yes" : "no");
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
  snprintf(line, sizeof(line), "  publish.build_failures=%lu", (unsigned long)_build_failures);
  out.println(line);
  snprintf(line, sizeof(line), "  publish.serialize_preflight_drops=%lu",
           (unsigned long)_serialize_preflight_drops);
  out.println(line);
  snprintf(line, sizeof(line), "  heap.free=%u", (unsigned int)ESP.getFreeHeap());
  out.println(line);
  snprintf(line, sizeof(line), "  heap.min_free=%u", (unsigned int)ESP.getMinFreeHeap());
  out.println(line);
  snprintf(line, sizeof(line), "  heap.min_seen_since_boot=%u", (unsigned int)_min_free_heap);
  out.println(line);
  snprintf(line, sizeof(line), "  heap.internal_free=%u largest_block=%u",
           (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  out.println(line);
  snprintf(line, sizeof(line), "  heap.psram_free=%u largest_block=%u",
           (unsigned int)(_psram_available
                              ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                              : 0),
           (unsigned int)(_psram_available
                              ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                              : 0));
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.connected=%s", isWiFiConnected() ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.got_ip=%s", _wifi_got_ip ? "yes" : "no");
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.last_disconnect_reason=%d", _last_wifi_disconnect_reason);
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.callback_queue_drops=%lu",
           (unsigned long)_wifi_callback_queue_drops.load(std::memory_order_relaxed));
  out.println(line);
  snprintf(line, sizeof(line), "  wifi.last_ip=%s", _wifi_last_ip.length() ? _wifi_last_ip.c_str() : "");
  out.println(line);
  snprintf(line, sizeof(line), "  clock.source=%s validated=%s fallback=%s age_ms=%lu",
           mqtt_clock::sourceName(_clock_source),
           mqtt_clock::isValidated(_clock_source) ? "yes" : "no",
           mqtt_clock::isFallback(_clock_source) ? "yes" : "no",
           (unsigned long)mqtt_clock::ageMs(
               _time_synced, (uint32_t)_ntp_synced_at_ms, (uint32_t)millis()));
  out.println(line);
  snprintf(line, sizeof(line), "  clock.validation_mode=%s attempt=%lu pending=%s",
           mqtt_clock::validationModeName(MQTT_NTP_VALIDATION_MODE),
           (unsigned long)_ntp_attempt_generation,
           _ntp_sync_pending ? "yes" : "no");
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
  callback.error_type = -1;
  callback.error_code = -1;
  callback.tls_last_error = -1;
  callback.tls_stack_error = -1;
  callback.tls_cert_verify_flags = -1;
  callback.transport_sock_errno = -1;
  callback.connect_return_code = -1;
  callback.client = event->client;
  callback.generation = ctx->generation;
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
      callback.error_code = -1;
      if (event->error_handle != nullptr) {
        callback.tls_last_error = (int)event->error_handle->esp_tls_last_esp_err;
        callback.tls_stack_error = event->error_handle->esp_tls_stack_err;
        callback.tls_cert_verify_flags = event->error_handle->esp_tls_cert_verify_flags;
        callback.transport_sock_errno = event->error_handle->esp_transport_sock_errno;
        callback.connect_return_code = (int)event->error_handle->connect_return_code;
        // The connect return code is only meaningful for refused connects;
        // the socket errno is the useful detail for transport failures.
        callback.error_code = (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                                  ? (int)event->error_handle->esp_transport_sock_errno
                                  : (int)event->error_handle->connect_return_code;
      }
      break;
    default:
      return ESP_OK;
  }
  ctx->reporter->enqueueCallbackEvent(callback);
  return ESP_OK;
}

bool MqttReporter::shouldIncludePath(const mesh::Packet *pkt) {
  if (pkt == nullptr || pkt->payload_len < 2) return false;
  uint8_t type = pkt->getPayloadType();
  return type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE || type == PAYLOAD_TYPE_TXT_MSG;
}

#endif
