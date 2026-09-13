#include "MqttPayloadBuilders.h"

#include "helpers/MqttWirePreflight.h"

#include <string.h>

namespace mqtt_payload {
namespace {

void appendUnsignedField(mqtt_publish::CheckedBufferBuilder &builder,
                         const char *key,
                         uint64_t value) {
  builder.append(key);
  builder.appendUnsigned(value);
}

void appendSignedField(mqtt_publish::CheckedBufferBuilder &builder,
                       const char *key,
                       int64_t value) {
  builder.append(key);
  builder.appendSigned(value);
}

void appendIdleField(mqtt_publish::CheckedBufferBuilder &builder,
                     const char *key,
                     float value) {
  builder.append(key);
  if (value < 0.0f) {
    builder.append("null");
  } else {
    builder.appendFloat1(value);
  }
}

}  // namespace

bool appendStatusStatsPayload(mqtt_publish::CheckedBufferBuilder &builder,
                              const StatusStatsInput &input) {
  builder.append("{");
  appendUnsignedField(builder, "\"uptime_ms\":", input.uptime_ms);
  appendUnsignedField(builder, ",\"boot_count\":", input.boot_count);
  builder.append(",\"reset_reason\":\"");
  builder.appendJsonEscaped(input.reset_reason);
  builder.append("\"");
  builder.append(",\"ntp_synced\":");
  builder.append(input.ntp_synced ? "true" : "false");
  builder.append(",\"clock_trusted\":");
  builder.append(input.clock_trusted ? "true" : "false");
  builder.append(",\"ntp_sync_source\":\"");
  builder.append(input.ntp_sync_source);
  builder.append("\"");
  builder.append(",\"ntp_sync_validated\":");
  builder.append(input.ntp_sync_validated ? "true" : "false");
  builder.append(",\"ntp_sync_fallback\":");
  builder.append(input.ntp_sync_fallback ? "true" : "false");
  builder.append(",\"ntp_validation_mode\":\"");
  builder.append(input.ntp_validation_mode);
  builder.append("\"");
  appendUnsignedField(builder, ",\"ntp_sync_age_ms\":", input.ntp_sync_age_ms);
  appendUnsignedField(builder, ",\"ntp_attempt_generation\":", input.ntp_attempt_generation);
  appendUnsignedField(builder, ",\"ntp_last_attempt_age_ms\":", input.ntp_last_attempt_age_ms);
  appendUnsignedField(builder, ",\"boot_epoch\":", input.boot_epoch);
  appendUnsignedField(builder, ",\"max_loop_ms\":", input.max_loop_ms);
  appendUnsignedField(builder, ",\"max_loop_at_ms\":", input.max_loop_at_ms);
  appendUnsignedField(builder, ",\"loop_iterations\":", input.loop_iterations);
  appendUnsignedField(builder, ",\"wifi_reconnect_attempts\":", input.wifi_reconnect_attempts);
  appendUnsignedField(builder, ",\"rx_publish_calls\":", input.rx_publish_calls);
  appendUnsignedField(builder, ",\"tx_publish_calls\":", input.tx_publish_calls);
  appendUnsignedField(builder, ",\"tx_fail_publish_calls\":", input.tx_fail_publish_calls);
  appendUnsignedField(builder, ",\"publish_skipped_no_connection\":",
                      input.publish_skipped_no_connection);
  appendUnsignedField(builder, ",\"build_failures\":", input.build_failures);
  appendUnsignedField(builder, ",\"serialize_preflight_drops\":",
                      input.serialize_preflight_drops);
  appendUnsignedField(builder, ",\"forward_successes\":", input.forward_successes);
  appendUnsignedField(builder, ",\"forward_successes_flood\":", input.forward_successes_flood);
  appendUnsignedField(builder, ",\"forward_successes_direct\":", input.forward_successes_direct);
  appendUnsignedField(builder, ",\"forward_failures\":", input.forward_failures);
  appendUnsignedField(builder, ",\"tx_queue_depth\":", input.tx_queue_depth);
  appendUnsignedField(builder, ",\"tx_queue_depth_peak\":", input.tx_queue_depth_peak);
  appendUnsignedField(builder, ",\"heap_free\":", input.heap_free);
  appendUnsignedField(builder, ",\"heap_min_free\":", input.heap_min_free);
  appendUnsignedField(builder, ",\"heap_min_seen_since_boot\":",
                      input.heap_min_seen_since_boot);
  appendUnsignedField(builder, ",\"heap_internal_free\":", input.heap_internal_free);
  appendUnsignedField(builder, ",\"heap_internal_largest_block\":",
                      input.heap_internal_largest_block);
  appendUnsignedField(builder, ",\"heap_psram_free\":", input.heap_psram_free);
  appendUnsignedField(builder, ",\"heap_psram_largest_block\":",
                      input.heap_psram_largest_block);
  builder.append(",\"wifi_connected\":");
  builder.append(input.wifi_connected ? "true" : "false");
  appendUnsignedField(builder, ",\"wifi_uptime_ms\":", input.wifi_uptime_ms);
  builder.append(",\"wifi_rssi\":");
  if (input.wifi_connected) {
    builder.appendSigned(input.wifi_rssi);
  } else {
    builder.append("null");
  }
  if (input.wifi_connected &&
      (input.wifi_ssid == nullptr || input.wifi_ssid[0] == '\0')) {
    return false;
  }
  builder.append(",\"wifi_ssid\":\"");
  builder.appendJsonEscaped(input.wifi_ssid);
  builder.append("\"");
  appendUnsignedField(builder, ",\"nodes_heard_24h\":", input.nodes_heard_24h);
  appendSignedField(builder, ",\"last_rx_rssi\":", input.last_rx_rssi);
  builder.append(",\"last_rx_snr\":");
  builder.appendFloat1(input.last_rx_snr);
  appendSignedField(builder, ",\"tx_power_dbm\":", input.tx_power_dbm);
  appendSignedField(builder, ",\"last_tx_fail_reason\":", input.last_tx_fail_reason);
  appendUnsignedField(builder, ",\"config_version\":", input.config_version);
  builder.append(",\"config_crc32\":\"");
  builder.append(input.config_crc32);
  builder.append("\"");
  builder.append(",\"fs_free_bytes\":");
  appendUnsignedField(builder, "", input.fs_free_bytes);
  appendUnsignedField(builder, ",\"fs_total_bytes\":", input.fs_total_bytes);
  if (input.nvs_free_entries_valid) {
    appendUnsignedField(builder, ",\"nvs_free_entries\":", input.nvs_free_entries);
  } else {
    builder.append(",\"nvs_free_entries\":null");
  }
  builder.append(",\"power_source\":\"");
  builder.append(input.power_source);
  builder.append("\"");
  builder.append(",\"solar_mv\":null");
  if (input.board_temp_valid) {
    builder.append(",\"board_temp_c\":");
    builder.appendFloat1(input.board_temp_c);
  } else {
    builder.append(",\"board_temp_c\":null");
  }
  if (input.channel_id != nullptr && strlen(input.channel_id) >= 8) {
    char short_channel_id[9];
    memcpy(short_channel_id, input.channel_id, 8);
    short_channel_id[8] = '\0';
    builder.append(",\"channel_id\":\"");
    builder.appendJsonEscaped(short_channel_id);
    builder.append("\"");
  } else {
    builder.append(",\"channel_id\":null");
  }
  builder.append(",\"git_commit\":\"");
  builder.appendJsonEscaped(input.git_commit);
  builder.append("\"");
  appendIdleField(builder, ",\"idle_pct_core0\":", input.idle_pct_core0);
  if (!input.idle_pct_core1_available) {
    builder.append(",\"idle_pct_core1\":0.0");
  } else {
    appendIdleField(builder, ",\"idle_pct_core1\":", input.idle_pct_core1);
  }

  static const char *const required_mesh_fields[] = {
      "\"battery_mv\":", "\"uptime_secs\":", "\"tx_air_secs\":",
      "\"rx_air_secs\":", "\"channel_utilization\":", "\"air_util_tx\":",
      "\"air_util_rx\":"};
  if (input.mesh_stats == nullptr || input.mesh_stats_len < 2 ||
      input.mesh_stats[0] != '{' || input.mesh_stats[input.mesh_stats_len - 1] != '}') {
    return false;
  }
  for (const char *required : required_mesh_fields) {
    if (strstr(input.mesh_stats, required) == nullptr) return false;
  }
  if (input.mesh_stats_len > 2) {
    builder.append(",");
    builder.append(input.mesh_stats + 1, input.mesh_stats_len - 2);
  }

  if (input.mqtt.present) {
    const StatusMqttInput &mqtt = input.mqtt;
    builder.append(",\"mqtt\":{");
    appendUnsignedField(builder, "\"broker_index\":", mqtt.broker_index);
    appendUnsignedField(builder, ",\"connect_attempts\":", mqtt.connect_attempts);
    appendUnsignedField(builder, ",\"connect_start_failures\":", mqtt.connect_start_failures);
    appendUnsignedField(builder, ",\"connect_events\":", mqtt.connect_events);
    appendUnsignedField(builder, ",\"disconnect_events\":", mqtt.disconnect_events);
    appendUnsignedField(builder, ",\"error_events\":", mqtt.error_events);
    appendUnsignedField(builder, ",\"reconnect_rung\":", mqtt.reconnect_rung);
    builder.append(",\"reconnect_breaker\":");
    builder.append(mqtt.reconnect_breaker ? "true" : "false");
    appendUnsignedField(builder, ",\"reconnect_next_in_ms\":", mqtt.reconnect_next_in_ms);
    appendSignedField(builder, ",\"last_error_type\":", mqtt.last_error_type);
    appendSignedField(builder, ",\"last_error_code\":", mqtt.last_error_code);
    appendSignedField(builder, ",\"last_tls_err\":", mqtt.last_tls_err);
    appendSignedField(builder, ",\"last_tls_stack_err\":", mqtt.last_tls_stack_err);
    appendSignedField(builder, ",\"last_tls_cert_verify_flags\":",
                      mqtt.last_tls_cert_verify_flags);
    appendSignedField(builder, ",\"last_sock_errno\":", mqtt.last_sock_errno);
    appendSignedField(builder, ",\"last_connect_return_code\":",
                      mqtt.last_connect_return_code);
    appendUnsignedField(builder, ",\"last_error_at_ms\":", mqtt.last_error_at_ms);
    appendUnsignedField(builder, ",\"last_error_age_ms\":", mqtt.last_error_age_ms);
    builder.append(",\"heap_inactive\":");
    builder.append(mqtt.heap_inactive ? "true" : "false");
    builder.append(",\"effective_transport\":\"");
    builder.append(mqtt.effective_transport);
    builder.append("\"");
    appendUnsignedField(builder, ",\"tls_live\":", mqtt.tls_live);
    appendUnsignedField(builder, ",\"tls_cap\":", mqtt.tls_cap);
    if (!mqtt.broker_uri_valid) return false;
    builder.append(",\"broker_uri\":\"");
    builder.appendJsonEscaped(mqtt.broker_uri);
    builder.append("\",\"broker_username\":\"");
    builder.appendJsonEscaped(mqtt.broker_username);
    builder.append("\"");
    appendUnsignedField(builder, ",\"reconnect_attempts_1h\":", mqtt.reconnect_attempts_1h);
    appendUnsignedField(builder, ",\"status_publishes\":", mqtt.status_publishes);
    appendUnsignedField(builder, ",\"packet_publishes\":", mqtt.packet_publishes);
    appendUnsignedField(builder, ",\"session_status_publishes\":",
                        mqtt.session_status_publishes);
    appendUnsignedField(builder, ",\"session_packet_publishes\":",
                        mqtt.session_packet_publishes);
    appendUnsignedField(builder, ",\"publish_failures\":", mqtt.publish_failures);
    appendUnsignedField(builder, ",\"publish_queue_depth\":", mqtt.publish_queue_depth);
    appendUnsignedField(builder, ",\"publish_queue_cap\":", mqtt.publish_queue_cap);
    appendUnsignedField(builder, ",\"publish_queue_drops\":", mqtt.publish_queue_drops);
    appendUnsignedField(builder, ",\"publish_queue_byte_drops\":",
                        mqtt.publish_queue_byte_drops);
    appendUnsignedField(builder, ",\"publish_queue_bytes\":", mqtt.publish_queue_bytes);
    appendUnsignedField(builder, ",\"publish_queue_byte_cap\":", mqtt.publish_queue_byte_cap);
    appendUnsignedField(builder, ",\"publish_queue_total_bytes\":",
                        mqtt.publish_queue_total_bytes);
    appendUnsignedField(builder, ",\"publish_queue_total_byte_cap\":",
                        mqtt.publish_queue_total_byte_cap);
    appendSignedField(builder, ",\"publish_outbox_size\":", mqtt.publish_outbox_size);
    appendUnsignedField(builder, ",\"publish_outbox_cap\":", mqtt.publish_outbox_cap);
    appendUnsignedField(builder, ",\"publish_outbox_drops\":", mqtt.publish_outbox_drops);
    builder.append(",\"connected\":");
    builder.append(mqtt.connected ? "true" : "false");
    appendUnsignedField(builder, ",\"uptime_ms\":", mqtt.uptime_ms);
    appendUnsignedField(builder, ",\"neighbor_interval_s\":", mqtt.neighbor_interval_s);
    appendUnsignedField(builder, ",\"last_offline_epoch\":", mqtt.last_offline_epoch);
    builder.append("}");
  }

  builder.append("}");
  return builder.ok();
}

bool buildStatusPayload(char *buffer,
                        size_t capacity,
                        const StatusPayloadInput &input,
                        size_t &payload_len) {
  payload_len = 0;
  mqtt_publish::CheckedBufferBuilder builder(buffer, capacity);
  builder.append("{");
  if (input.status != nullptr && input.status[0] != '\0') {
    builder.append("\"status\":\"");
    builder.appendJsonEscaped(input.status);
    builder.append("\",");
  }
  builder.append("\"origin\":\"");
  builder.appendJsonEscaped(input.origin);
  builder.append("\",\"origin_id\":\"");
  builder.append(input.origin_id);
  builder.append("\",\"model\":\"");
  builder.appendJsonEscaped(input.model);
  builder.append("\",\"firmware_version\":\"");
  builder.appendJsonEscaped(input.firmware_version);
  builder.append("\",\"radio\":\"");
  builder.appendJsonEscaped(input.radio);
  builder.append("\",\"client_version\":\"");
  builder.appendJsonEscaped(input.client_version);
  builder.append("\"");
  if (input.time_synced) {
    builder.append(",\"timestamp\":\"");
    builder.append(input.timestamp);
    builder.append("\"");
  } else {
    builder.append(",\"timestamp\":null");
  }
  builder.append(",\"stats\":");
  if (input.stats_appender == nullptr ||
      !input.stats_appender(builder, input.broker_idx, input.stats_context)) {
    return false;
  }
  builder.append("}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxLwtPayloadBytes) return false;
  payload_len = builder.length();
  return true;
}

bool buildPacketPayload(char *buffer,
                        size_t capacity,
                        const PacketPayloadInput &input,
                        size_t &payload_len) {
  payload_len = 0;
  if (input.direction == nullptr || input.origin == nullptr ||
      input.origin_id == nullptr || input.raw == nullptr ||
      input.raw_len > input.max_raw_bytes) {
    return false;
  }
  const mqtt_wire::Preflight preflight = mqtt_wire::validate(
      input.packet_payload_len,
      input.encoded_path_len,
      input.has_transport_codes,
      input.max_packet_payload,
      input.max_path_bytes,
      input.max_raw_bytes);
  if (!preflight.ok()) return false;

  mqtt_publish::CheckedBufferBuilder builder(buffer, capacity);
  builder.append("{\"origin\":\"");
  builder.appendJsonEscaped(input.origin);
  builder.append("\",\"origin_id\":\"");
  builder.append(input.origin_id);
  builder.append("\"");
  if (input.time_synced) {
    builder.append(",\"timestamp\":\"");
    builder.append(input.timestamp);
    builder.append("\"");
  } else {
    builder.append(",\"timestamp\":null");
  }
  builder.append(",\"type\":\"PACKET\",\"direction\":\"");
  builder.appendJsonEscaped(input.direction);
  builder.append("\"");
  if (input.time_synced) {
    builder.append(",\"time\":\"");
    builder.append(input.time);
    builder.append("\",\"date\":\"");
    builder.append(input.date);
    builder.append("\"");
  } else {
    builder.append(",\"time\":null,\"date\":null");
  }
  builder.append(",\"len\":\"");
  builder.appendSigned(input.len);
  builder.append("\",\"packet_type\":\"");
  builder.appendUnsigned(input.packet_type);
  builder.append("\",\"route\":\"");
  builder.append(input.route_direct ? "D" : "F");
  builder.append("\",\"payload_len\":\"");
  builder.appendUnsigned(input.packet_payload_len);
  builder.append("\",\"raw\":\"");
  builder.appendHex(input.raw, input.raw_len);
  builder.append("\"");
  if (input.include_radio_metrics) {
    builder.append(",\"SNR\":\"");
    builder.appendSigned(input.snr);
    builder.append("\",\"RSSI\":\"");
    builder.appendSigned(input.rssi);
    builder.append("\",\"score\":\"");
    builder.appendSigned(input.score);
    builder.append("\",\"duration\":\"");
    builder.appendUnsigned(input.duration_ms);
    builder.append("\"");
  }
  builder.append(",\"hash\":\"");
  builder.appendHex(input.hash, input.hash_len);
  builder.append("\"");
  if (input.route_direct && input.include_path) {
    builder.append(",\"path\":\"");
    builder.append(input.path);
    builder.append("\"");
  }
  builder.append("}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxPublishPayloadBytes) return false;
  payload_len = builder.length();
  return true;
}

bool buildNeighborsPayload(char *buffer,
                           size_t capacity,
                           const NeighborsPayloadInput &input,
                           size_t &payload_len) {
  payload_len = 0;
  if (input.reader == nullptr) return false;
  mqtt_publish::CheckedBufferBuilder builder(buffer, capacity);
  builder.append("{\"nodes\":[");
  bool first = true;
  for (size_t i = 0; i < input.capacity; ++i) {
    NeighborRecord record = {};
    if (!input.reader(input.context, i, record)) return false;
    if (!record.used) continue;
    if (!first) builder.append(",");
    first = false;
    builder.append("{\"id\":\"");
    builder.appendHex(record.id, record.id_bytes);
    builder.append("\",\"rssi\":");
    builder.appendSigned(record.rssi);
    builder.append(",\"snr\":");
    builder.appendFloat1(record.snr);
    builder.append(",\"last_seen_s\":");
    builder.appendUnsigned((uint32_t)(input.now_ms - record.last_heard_ms) / 1000UL);
    builder.append("}");
  }
  builder.append("]}");
  if (!builder.ok() || builder.length() > mqtt_publish::kMaxPublishPayloadBytes) return false;
  payload_len = builder.length();
  return true;
}

}  // namespace mqtt_payload
