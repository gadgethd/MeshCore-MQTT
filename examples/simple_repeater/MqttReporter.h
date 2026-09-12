#pragma once

#if defined(ESP32) && defined(WITH_MQTT_REPORTER)

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <WiFi.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>
#include "MqttSettings.h"
#include "helpers/MQTTLifecycle.h"
#include "helpers/MqttClockPolicy.h"
#include "helpers/MqttPublishGuard.h"
#include "helpers/MqttReconnectPolicy.h"
#include "helpers/MqttTlsCapPolicy.h"
#include "helpers/MqttWirePreflight.h"

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

#ifndef MQTT_CALLBACK_QUEUE_DEPTH
  #define MQTT_CALLBACK_QUEUE_DEPTH 32
#endif

class MqttReporter : private MQTTLifecycle::Ops {
public:
  MqttReporter(MyMesh &mesh, mesh::RTCClock &clock);
  ~MqttReporter();

  void begin(FILESYSTEM *fs);
  bool end();
  void loop();

  // OTA lifecycle seam used by ESP32Board. stopForOTA() returns true only when
  // ordered client teardown acknowledged inside the audited timeout. A dirty
  // or unproven stop remains fail-closed; resume is possible only after proof.
  bool stopForOTA();
  bool resumeAfterOTAAbort();
  bool canFlashAfterStop() const;
  static MqttReporter *activeInstance();

  void publishRxRaw(const uint8_t raw[], int len);
  void clearPendingRxRaw();
  void publishRxPacket(mesh::Packet *pkt, int len, float score, int rssi, float snr, uint32_t duration_ms);
  void publishTxPacket(mesh::Packet *pkt, int len);
  void publishTxFail(mesh::Packet *pkt, int len, int reason = 0);

  const char *getWiFiSsid() const;
  bool isWiFiConnected() const;
  bool isMqttConnected() const;
  bool isMqttConnected(int broker_idx) const;
  bool getConfigValue(const char *key, char *dest, size_t dest_size, bool mask_secret = true) const;
  bool setConfigValue(const char *key, const char *value);
  bool resetConfig();
  void reconnect(int broker_idx = -1);
  void printConfig(Print &out, int broker_idx = -1) const;
  void printStats(Print &out, int broker_idx = -1) const;

private:
  static constexpr uint8_t MQTT_PUBLISH_QUEUE_SIZE = MQTT_PUBLISH_QUEUE_DEPTH;
  static constexpr uint8_t MQTT_PUBLISHES_PER_LOOP = MQTT_MAX_PUBLISHES_PER_LOOP;
  static constexpr uint8_t MQTT_RECONNECT_RING_SIZE = 32;
  static constexpr uint8_t HEARD_NODE_CAPACITY = 128;
  static constexpr uint8_t NEIGHBOR_CAPACITY = 32;
  static constexpr uint32_t HEARD_NODE_WINDOW_MS = 24UL * 60UL * 60UL * 1000UL;
  static constexpr uint32_t RECONNECT_WINDOW_MS = 60UL * 60UL * 1000UL;
  static_assert(MQTT_PUBLISH_QUEUE_DEPTH > 0 && MQTT_PUBLISH_QUEUE_DEPTH <= UINT8_MAX,
                "MQTT_PUBLISH_QUEUE_DEPTH must be between 1 and 255");
  static_assert(MQTT_MAX_PUBLISHES_PER_LOOP > 0 && MQTT_MAX_PUBLISHES_PER_LOOP <= UINT8_MAX,
                "MQTT_MAX_PUBLISHES_PER_LOOP must be between 1 and 255");
  static_assert(384 - 1 == mqtt_publish::kMaxStatusTopicBytes,
                "status topic and CONNECT-size contract drifted");

  struct PublishEntry {
    char *topic;
    char *payload;
    size_t topic_len;
    size_t payload_len;
    size_t copied_bytes;
    uint8_t qos;
    bool retain;
    bool is_status;
    bool pending;
  };

  struct BrokerClient {
    esp_mqtt_client_handle_t client;
    std::atomic<uint32_t> client_generation;
    std::atomic<uint32_t> pending_connected_generation;
    std::atomic<uint32_t> pending_terminal_generation;
    std::atomic<uint32_t> callback_queue_drops;
    std::atomic<uint8_t> pending_terminal_type;
    std::atomic<int> pending_error_type;
    std::atomic<int> pending_error_code;
    std::atomic<int> pending_tls_last_error;
    std::atomic<int> pending_tls_stack_error;
    std::atomic<int> pending_tls_cert_verify_flags;
    std::atomic<int> pending_transport_sock_errno;
    std::atomic<int> pending_connect_return_code;
    volatile bool started;
    volatile bool connected;
    mqtt_tls_cap::Transport effective_transport;
    uint32_t next_connect_attempt_ms;
    unsigned long connected_since_ms;
    char status_topic[384];
    char packets_topic[384];
    char neighbors_topic[384];
    char *offline_payload;
    size_t offline_payload_len;
    unsigned long last_status_publish;
    unsigned long last_neighbors_publish;
    PublishEntry publish_queue[MQTT_PUBLISH_QUEUE_SIZE];
    uint8_t queue_head;
    uint8_t queue_tail;
    uint8_t queue_count;
    size_t queue_bytes;
    bool status_queued;
    volatile bool online_status_pending;
    uint32_t connect_attempts;
    uint32_t connect_start_failures;
    uint32_t connect_events;
    uint32_t disconnect_events;
    uint32_t error_events;
    uint32_t status_publish_count;
    uint32_t packet_publish_count;
    uint32_t session_status_publish_count;
    uint32_t session_packet_publish_count;
    uint32_t publish_failures;
    uint32_t queue_drops;
    uint32_t queue_byte_drops;
    uint32_t outbox_drops;
    uint32_t reconnect_attempt_ms[MQTT_RECONNECT_RING_SIZE];
    uint8_t reconnect_attempt_head;
    uint8_t reconnect_attempt_count;
    uint32_t last_offline_epoch;
    bool heap_inactive;
    int last_error_type;
    int last_error_code;
    int last_tls_last_error;
    int last_tls_stack_error;
    int last_tls_cert_verify_flags;
    int last_transport_sock_errno;
    int last_connect_return_code;
    uint32_t last_error_at_ms;
    mqtt_reconnect::State recon;
    bool recon_seeded;
  };

  struct HeardNodeEntry {
    uint8_t id[PUB_KEY_SIZE];
    uint32_t last_heard_ms;
    bool used;
  };

  struct NeighborEntry {
    uint8_t id[PUB_KEY_SIZE];
    int rssi;
    float snr;
    uint32_t last_heard_ms;
    bool used;
  };

  struct EventContext {
    MqttReporter *reporter;
    int broker_idx;
    uint32_t generation;
  };

  enum class CallbackEventType : uint8_t {
    WifiDisconnected,
    WifiGotIp,
    MqttConnected,
    MqttDisconnected,
    MqttError
  };

  struct CallbackEvent {
    CallbackEventType type;
    int8_t broker_idx;
    int wifi_reason;
    int error_type;
    int error_code;
    int tls_last_error;
    int tls_stack_error;
    int tls_cert_verify_flags;
    int transport_sock_errno;
    int connect_return_code;
    esp_mqtt_client_handle_t client;
    uint32_t generation;
  };

  MyMesh *_mesh;
  mesh::RTCClock *_clock;
  MqttSettingsStore _settings;
  MQTTLifecycle::Coordinator _lifecycle;
  FILESYSTEM *_filesystem;
  bool _settings_initialized;
  bool _wifi_event_registered;
  bool _stop_requested;
  std::atomic<bool> _callbacks_allowed;
  std::atomic<bool> _flash_allowed;
  BrokerClient _clients[MQTT_MAX_BROKERS];
  EventContext _event_ctx[MQTT_MAX_BROKERS];
  QueueHandle_t _callback_queue;
  mqtt_reconnect::AttemptGuard _reconnect_guard;
  std::atomic<uint8_t> _pending_wifi_event;
  std::atomic<int> _pending_wifi_disconnect_reason;
  std::atomic<uint32_t> _wifi_callback_queue_drops;
  uint8_t _last_rx_raw[MAX_TRANS_UNIT];
  size_t _last_rx_raw_len;
  unsigned long _last_wifi_attempt;
  bool _wifi_attempted;
  unsigned long _last_stats_print;
  unsigned long _last_ntp_attempt;
  bool _ntp_attempted;
  unsigned long _ntp_sync_started_at;
  bool _ntp_sync_pending;
  uint32_t _ntp_attempt_generation;
  uint32_t _ntp_pending_generation;
  bool _time_synced;
  unsigned long _ntp_synced_at_ms;
  uint8_t _ntp_failures;
  mqtt_clock::Source _clock_source;
  char _origin_id[65];
  char _client_id[40];
  char _reset_reason[20];
  uint32_t _boot_count;
  uint32_t _config_crc32;
  uint32_t _max_loop_ms;
  uint32_t _max_loop_at_ms;
  int _last_rx_rssi;
  float _last_rx_snr;
  int _last_tx_fail_reason;
  uint32_t _rx_publish_calls;
  uint32_t _tx_publish_calls;
  uint32_t _tx_fail_publish_calls;
  uint32_t _publish_skipped_no_connection;
  uint32_t _build_failures;
  uint32_t _serialize_preflight_drops;
  size_t _publish_queue_bytes_total;
  char *_build_buffer;
  bool _psram_available;
  uint8_t _next_publish_broker;
  uint32_t _wifi_reconnect_attempts;
  uint8_t _wifi_consecutive_failures;
  uint32_t _loop_iterations;
  uint32_t _min_free_heap;
  int _last_wifi_disconnect_reason;
  bool _wifi_got_ip;
  unsigned long _wifi_connected_since_ms;
  String _wifi_last_ip;
  bool _identity_strings_dirty;
  unsigned long _last_cpu_sample_ms;
  float _idle_pct_core0;
  float _idle_pct_core1;
  uint32_t _last_idle_tick_count[2];
  uint32_t _last_total_runtime;
  HeardNodeEntry _heard_nodes[HEARD_NODE_CAPACITY];
  NeighborEntry _neighbors[NEIGHBOR_CAPACITY];

  void ensureIdentityStrings();
  uint8_t ownedClientCount() const;
  void serviceStopRequest();

  // MQTTLifecycle::Ops. These run only on the reporter's owner loop task;
  // callbacks observe the atomically published callback/flash gates.
  uint32_t nowMs() override;
  void startClients() override;
  void deliverStop() override;
  void releaseResources() override;
  void onStopComplete(bool clean) override;

  void resetBrokerConnection(int idx);
  void resetAllConnections();
  bool connectWiFi();
  bool connectMQTT(int idx);
  void processBrokerReconnects(uint32_t now_ms);
  void syncTimeFromNtp();
  void checkNtpSyncComplete();
  void stopNtpService();
  void recordClockAcceptance(const mqtt_clock::Acceptance &decision, uint32_t now_ms);
  bool acceptExistingClockFallback(const char *reason);
  bool tlsBudgetAllows(int broker_idx, mqtt_tls_cap::Transport requested) const;
  uint8_t liveTlsCount() const;
  uint8_t tlsCap() const;
  void publishStatus(int idx, const char *status);
  bool enqueuePublish(int idx, const char *topic, const char *payload, size_t payload_len,
                      uint8_t qos, bool retain, bool is_status);
  void drainPublishQueue(int idx, mqtt_publish::DrainState &drain);
  void clearPublishQueue(int idx);
  bool anyBrokerConnected() const;
  void enqueueCallbackEvent(const CallbackEvent &event);
  void processCallbackEvents();
  void handleWifiEvent(const CallbackEvent &event);
  void processPendingTerminalEvents();
  void handleMqttEvent(const CallbackEvent &event);
  bool reconcileMqttTerminal(int broker_idx, uint32_t now_ms);
  bool brokerNeedsTimeSync(int idx) const;
  void finishLoop(int64_t started_us);
  void recordReconnectAttempt(BrokerClient &bc, uint32_t now_ms);
  uint32_t reconnectAttemptsLastHour(const BrokerClient &bc, uint32_t now_ms) const;
  void upsertHeardNode(const uint8_t id[PUB_KEY_SIZE], uint32_t now_ms);
  void upsertNeighbor(const uint8_t id[PUB_KEY_SIZE], int rssi, float snr, uint32_t now_ms);
  uint32_t heardNodesLast24Hours(uint32_t now_ms) const;
  bool buildNeighborsPayload(uint32_t now_ms, size_t &payload_len) const;
  void maybePublishNeighbors(uint32_t now_ms);

  void appendCpuIdleStats(mqtt_publish::CheckedBufferBuilder &builder) const;
  bool appendStatusStatsPayload(mqtt_publish::CheckedBufferBuilder &builder, int broker_idx) const;
  bool buildStatusPayload(int broker_idx, const char *status, size_t &payload_len) const;
  bool buildPacketPayload(
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
      size_t &payload_len) const;

  char *allocateReporterBuffer(size_t bytes, bool prefer_psram) const;
  void releaseReporterBuffer(char *buffer) const;
  bool replaceOfflinePayload(BrokerClient &bc, const char *payload, size_t payload_len);
  void releasePublishEntry(PublishEntry &entry);
  size_t publishQueueBrokerByteCap() const;
  size_t publishQueueTotalByteCap() const;

  void printBrokerConfig(Print &out, int idx) const;
  void printBrokerStats(Print &out, int idx) const;
  void maybePrintPeriodicStats();

  static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event);
  static bool shouldIncludePath(const mesh::Packet *pkt);
  static std::atomic<MqttReporter *> s_instance;
};

#endif
