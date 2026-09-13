#pragma once

#include <stddef.h>
#include <stdint.h>

#include "helpers/MqttPublishGuard.h"

// Dependency-free payload construction shared by the ESP32 reporter and the
// native production-builder tests. Runtime reads stay in MqttReporter; this
// unit owns the wire shape and checked serialization.
namespace mqtt_payload {

struct StatusMqttInput {
  bool present;
  uint64_t broker_index;
  uint64_t connect_attempts;
  uint64_t connect_start_failures;
  uint64_t connect_events;
  uint64_t disconnect_events;
  uint64_t error_events;
  uint64_t reconnect_rung;
  bool reconnect_breaker;
  uint64_t reconnect_next_in_ms;
  int64_t last_error_type;
  int64_t last_error_code;
  int64_t last_tls_err;
  int64_t last_tls_stack_err;
  int64_t last_tls_cert_verify_flags;
  int64_t last_sock_errno;
  int64_t last_connect_return_code;
  uint64_t last_error_at_ms;
  uint64_t last_error_age_ms;
  bool heap_inactive;
  const char *effective_transport;
  uint64_t tls_live;
  uint64_t tls_cap;
  const char *broker_uri;
  bool broker_uri_valid;
  const char *broker_username;
  uint64_t reconnect_attempts_1h;
  uint64_t status_publishes;
  uint64_t packet_publishes;
  uint64_t session_status_publishes;
  uint64_t session_packet_publishes;
  uint64_t publish_failures;
  uint64_t publish_queue_depth;
  uint64_t publish_queue_cap;
  uint64_t publish_queue_drops;
  uint64_t publish_queue_byte_drops;
  uint64_t publish_queue_bytes;
  uint64_t publish_queue_byte_cap;
  uint64_t publish_queue_total_bytes;
  uint64_t publish_queue_total_byte_cap;
  int64_t publish_outbox_size;
  uint64_t publish_outbox_cap;
  uint64_t publish_outbox_drops;
  bool connected;
  uint64_t uptime_ms;
  uint64_t neighbor_interval_s;
  uint64_t last_offline_epoch;
};

struct StatusStatsInput {
  uint64_t uptime_ms;
  uint64_t boot_count;
  const char *reset_reason;
  bool ntp_synced;
  bool clock_trusted;
  const char *ntp_sync_source;
  bool ntp_sync_validated;
  bool ntp_sync_fallback;
  const char *ntp_validation_mode;
  uint64_t ntp_sync_age_ms;
  uint64_t ntp_attempt_generation;
  uint64_t ntp_last_attempt_age_ms;
  uint64_t boot_epoch;
  uint64_t max_loop_ms;
  uint64_t max_loop_at_ms;
  uint64_t loop_iterations;
  uint64_t wifi_reconnect_attempts;
  uint64_t rx_publish_calls;
  uint64_t tx_publish_calls;
  uint64_t tx_fail_publish_calls;
  uint64_t publish_skipped_no_connection;
  uint64_t build_failures;
  uint64_t serialize_preflight_drops;
  uint64_t forward_successes;
  uint64_t forward_successes_flood;
  uint64_t forward_successes_direct;
  uint64_t forward_failures;
  uint64_t tx_queue_depth;
  uint64_t tx_queue_depth_peak;
  uint64_t heap_free;
  uint64_t heap_min_free;
  uint64_t heap_min_seen_since_boot;
  uint64_t heap_internal_free;
  uint64_t heap_internal_largest_block;
  uint64_t heap_psram_free;
  uint64_t heap_psram_largest_block;
  bool wifi_connected;
  uint64_t wifi_uptime_ms;
  int64_t wifi_rssi;
  const char *wifi_ssid;
  uint64_t nodes_heard_24h;
  int64_t last_rx_rssi;
  float last_rx_snr;
  int64_t tx_power_dbm;
  int64_t last_tx_fail_reason;
  uint64_t config_version;
  const char *config_crc32;
  uint64_t fs_free_bytes;
  uint64_t fs_total_bytes;
  bool nvs_free_entries_valid;
  uint64_t nvs_free_entries;
  const char *power_source;
  bool board_temp_valid;
  float board_temp_c;
  const char *channel_id;
  const char *git_commit;
  float idle_pct_core0;
  bool idle_pct_core1_available;
  float idle_pct_core1;
  const char *mesh_stats;
  size_t mesh_stats_len;
  StatusMqttInput mqtt;
};

using StatusStatsAppender = bool (*)(
    mqtt_publish::CheckedBufferBuilder &builder,
    int broker_idx,
    const void *context);

struct StatusPayloadInput {
  const char *status;
  const char *origin;
  const char *origin_id;
  const char *model;
  const char *firmware_version;
  const char *radio;
  const char *client_version;
  bool time_synced;
  const char *timestamp;
  int broker_idx;
  StatusStatsAppender stats_appender;
  const void *stats_context;
};

struct PacketPayloadInput {
  const char *direction;
  const char *origin;
  const char *origin_id;
  bool time_synced;
  const char *timestamp;
  const char *time;
  const char *date;
  int64_t len;
  uint64_t packet_type;
  bool route_direct;
  uint64_t packet_payload_len;
  const uint8_t *raw;
  size_t raw_len;
  bool include_radio_metrics;
  int64_t snr;
  int64_t rssi;
  int64_t score;
  uint64_t duration_ms;
  const uint8_t *hash;
  size_t hash_len;
  bool include_path;
  const char *path;
  size_t max_packet_payload;
  size_t encoded_path_len;
  bool has_transport_codes;
  size_t max_path_bytes;
  size_t max_raw_bytes;
};

struct NeighborRecord {
  const uint8_t *id;
  size_t id_bytes;
  int64_t rssi;
  float snr;
  uint32_t last_heard_ms;
  bool used;
};

using NeighborReader = bool (*)(const void *context,
                                size_t index,
                                NeighborRecord &record);

struct NeighborsPayloadInput {
  uint32_t now_ms;
  size_t capacity;
  NeighborReader reader;
  const void *context;
};

bool appendStatusStatsPayload(mqtt_publish::CheckedBufferBuilder &builder,
                              const StatusStatsInput &input);

bool buildStatusPayload(char *buffer,
                        size_t capacity,
                        const StatusPayloadInput &input,
                        size_t &payload_len);

bool buildPacketPayload(char *buffer,
                        size_t capacity,
                        const PacketPayloadInput &input,
                        size_t &payload_len);

bool buildNeighborsPayload(char *buffer,
                           size_t capacity,
                           const NeighborsPayloadInput &input,
                           size_t &payload_len);

}  // namespace mqtt_payload
