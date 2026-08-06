#pragma once

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <WiFi.h>
#include <atomic>
#include <mqtt_client.h>
#include "MqttSettings.h"

class MyMesh;

#ifndef LORA_FREQ
  #define LORA_FREQ 869.525
#endif

#ifndef LORA_BW
  #define LORA_BW 62.5
#endif

#ifndef LORA_SF
  #define LORA_SF 8
#endif

#ifndef LORA_CR
  #define LORA_CR 5
#endif

#ifndef MQTT_PUBLISH_QUEUE_DEPTH
  #define MQTT_PUBLISH_QUEUE_DEPTH 16
#endif

#ifndef MQTT_MAX_PUBLISHES_PER_LOOP
  #define MQTT_MAX_PUBLISHES_PER_LOOP 2
#endif

class MqttReporter {
public:
  MqttReporter(MyMesh &mesh, mesh::RTCClock &clock);
  ~MqttReporter();

  void begin(FILESYSTEM *fs);
  void loop();

  void publishRxRaw(const uint8_t raw[], int len);
  void publishRxPacket(mesh::Packet *pkt, int len, float score, int rssi, float snr, uint32_t duration_ms);
  void publishTxPacket(mesh::Packet *pkt, int len);
  void publishTxFail(mesh::Packet *pkt, int len);

  const char *getWiFiSsid() const;
  bool isWiFiConnected() const;
  bool isMqttConnected() const;
  bool isMqttConnected(int broker_idx) const;
  bool getConfigValue(const char *key, char *dest, size_t dest_size, bool mask_secret = false) const;
  bool setConfigValue(const char *key, const char *value);
  bool resetConfig();
  void reconnect(int broker_idx = -1);
  void printConfig(Print &out, int broker_idx = -1) const;
  void printStats(Print &out, int broker_idx = -1) const;

private:
  static constexpr uint8_t MQTT_PUBLISH_QUEUE_SIZE = MQTT_PUBLISH_QUEUE_DEPTH;
  static constexpr uint8_t MQTT_PUBLISHES_PER_LOOP = MQTT_MAX_PUBLISHES_PER_LOOP;
  static_assert(MQTT_PUBLISH_QUEUE_DEPTH > 0 && MQTT_PUBLISH_QUEUE_DEPTH <= UINT8_MAX,
                "MQTT_PUBLISH_QUEUE_DEPTH must be between 1 and 255");
  static_assert(MQTT_MAX_PUBLISHES_PER_LOOP > 0 && MQTT_MAX_PUBLISHES_PER_LOOP <= UINT8_MAX,
                "MQTT_MAX_PUBLISHES_PER_LOOP must be between 1 and 255");

  struct PublishEntry {
    String topic;
    String payload;
    uint8_t qos;
    bool retain;
    bool is_status;
    bool pending;
  };

  struct BrokerClient {
    esp_mqtt_client_handle_t client;
    volatile bool started;
    volatile bool connected;
    char status_topic[384];
    char packets_topic[384];
    String offline_payload;
    unsigned long last_status_publish;
    PublishEntry publish_queue[MQTT_PUBLISH_QUEUE_SIZE];
    uint8_t queue_head;
    uint8_t queue_tail;
    uint8_t queue_count;
    bool status_queued;
    volatile bool online_status_pending;
    uint32_t connect_attempts;
    uint32_t connect_start_failures;
    uint32_t connect_events;
    uint32_t disconnect_events;
    uint32_t error_events;
    uint32_t status_publish_count;
    uint32_t packet_publish_count;
    uint32_t publish_failures;
    uint32_t queue_drops;
  };

  struct EventContext {
    MqttReporter *reporter;
    int broker_idx;
  };

  MyMesh *_mesh;
  mesh::RTCClock *_clock;
  MqttSettingsStore _settings;
  BrokerClient _clients[MQTT_MAX_BROKERS];
  EventContext _event_ctx[MQTT_MAX_BROKERS];
  String _last_rx_raw;
  unsigned long _last_wifi_attempt;
  bool _wifi_attempted;
  unsigned long _last_stats_print;
  unsigned long _last_ntp_attempt;
  bool _ntp_attempted;
  unsigned long _ntp_sync_started_at;
  bool _ntp_sync_pending;
  bool _time_synced;
  char _origin_id[65];
  char _client_id[40];
  uint32_t _rx_publish_calls;
  uint32_t _tx_publish_calls;
  uint32_t _tx_fail_publish_calls;
  uint32_t _publish_skipped_no_connection;
  uint32_t _wifi_reconnect_attempts;
  uint8_t _wifi_consecutive_failures;
  uint32_t _loop_iterations;
  uint32_t _min_free_heap;
  int _last_wifi_disconnect_reason;
  bool _wifi_got_ip;
  String _wifi_last_ip;
  unsigned long _last_cpu_sample_ms;
  float _idle_pct_core0;
  float _idle_pct_core1;
  uint32_t _last_idle_tick_count[2];
  uint32_t _last_total_runtime;

  void ensureIdentityStrings();
  void resetBrokerConnection(int idx);
  void resetAllConnections();
  bool connectWiFi();
  bool connectMQTT(int idx);
  void syncTimeFromNtp();
  void checkNtpSyncComplete();
  void publishStatus(int idx, const char *status);
  bool enqueuePublish(int idx, const char *topic, const String &payload, uint8_t qos, bool retain, bool is_status);
  void drainPublishQueue(int idx, uint8_t max_publishes = MQTT_PUBLISHES_PER_LOOP);
  void clearPublishQueue(int idx);
  bool anyBrokerConnected() const;
  void handleMqttEvent(int broker_idx, esp_mqtt_event_handle_t event);
  bool brokerNeedsTimeSync(int idx) const;

  String buildIsoTimestamp() const;
  String buildTimeField() const;
  String buildDateField() const;
  String buildRadioString() const;
  void appendCpuIdleStats(String &stats) const;
  String buildStatusPayload(int broker_idx, const char *status) const;
  String buildStatusStatsPayload(int broker_idx) const;
  String buildPacketPayload(
      const char *direction,
      mesh::Packet *pkt,
      int len,
      const String &raw_hex,
      float score,
      int rssi,
      float snr,
      uint32_t duration_ms,
      bool include_radio_metrics) const;

  void printBrokerConfig(Print &out, int idx) const;
  void printBrokerStats(Print &out, int idx) const;
  void maybePrintPeriodicStats();

  static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event);
  static String jsonEscape(const char *input);
  static String bytesToHex(const uint8_t *data, size_t len);
  static bool shouldIncludePath(const mesh::Packet *pkt);
  static std::atomic<MqttReporter *> s_instance;
};

#endif
