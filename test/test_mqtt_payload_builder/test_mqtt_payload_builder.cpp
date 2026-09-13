#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "helpers/MqttPayloadBuilders.h"

namespace {

using mqtt_payload::StatusStatsInput;
using mqtt_publish::CheckedBufferBuilder;

constexpr char kOriginId[] =
    "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";

StatusStatsInput fullStats() {
  StatusStatsInput stats = {};
  stats.uptime_ms = 123456;
  stats.boot_count = 7;
  stats.reset_reason = "rst\"reason";
  stats.ntp_synced = true;
  stats.clock_trusted = true;
  stats.ntp_sync_source = "ntp";
  stats.ntp_sync_validated = true;
  stats.ntp_sync_fallback = false;
  stats.ntp_validation_mode = "validated_exchange";
  stats.ntp_sync_age_ms = 1200;
  stats.ntp_attempt_generation = 9;
  stats.ntp_last_attempt_age_ms = 3400;
  stats.boot_epoch = 1700000000;
  stats.max_loop_ms = 12;
  stats.max_loop_at_ms = 9876;
  stats.loop_iterations = 456;
  stats.wifi_reconnect_attempts = 2;
  stats.rx_publish_calls = 3;
  stats.tx_publish_calls = 4;
  stats.tx_fail_publish_calls = 5;
  stats.publish_skipped_no_connection = 6;
  stats.build_failures = 8;
  stats.serialize_preflight_drops = 10;
  stats.forward_successes = 11;
  stats.forward_successes_flood = 12;
  stats.forward_successes_direct = 13;
  stats.forward_failures = 14;
  stats.tx_queue_depth = 1;
  stats.tx_queue_depth_peak = 2;
  stats.heap_free = 300000;
  stats.heap_min_free = 290000;
  stats.heap_min_seen_since_boot = 280000;
  stats.heap_internal_free = 270000;
  stats.heap_internal_largest_block = 260000;
  stats.heap_psram_free = 250000;
  stats.heap_psram_largest_block = 240000;
  stats.wifi_connected = true;
  stats.wifi_uptime_ms = 5000;
  stats.wifi_rssi = -55;
  stats.wifi_ssid = "lab\"ssid";
  stats.nodes_heard_24h = 15;
  stats.last_rx_rssi = -90;
  stats.last_rx_snr = 7.5f;
  stats.tx_power_dbm = 22;
  stats.last_tx_fail_reason = -3;
  stats.config_version = 16;
  stats.config_crc32 = "0x1234ABCD";
  stats.fs_free_bytes = 1000;
  stats.fs_total_bytes = 2000;
  stats.nvs_free_entries_valid = true;
  stats.nvs_free_entries = 99;
  stats.power_source = "usb";
  stats.board_temp_valid = true;
  stats.board_temp_c = 24.5f;
  stats.channel_id = "ABCDEF0123456789";
  stats.git_commit = "deadbeef";
  stats.idle_pct_core0 = 87.5f;
  stats.idle_pct_core1_available = true;
  stats.idle_pct_core1 = 86.5f;
  stats.mesh_stats =
      "{\"battery_mv\":4200,\"uptime_secs\":123,\"tx_air_secs\":4,"
      "\"rx_air_secs\":5,\"channel_utilization\":1.2,\"air_util_tx\":0.3,"
      "\"air_util_rx\":0.4}";
  stats.mesh_stats_len = std::strlen(stats.mesh_stats);

  stats.mqtt.present = true;
  stats.mqtt.broker_index = 1;
  stats.mqtt.connect_attempts = 2;
  stats.mqtt.connect_start_failures = 3;
  stats.mqtt.connect_events = 4;
  stats.mqtt.disconnect_events = 5;
  stats.mqtt.error_events = 6;
  stats.mqtt.reconnect_rung = 7;
  stats.mqtt.reconnect_breaker = false;
  stats.mqtt.reconnect_next_in_ms = 800;
  stats.mqtt.last_error_type = -1;
  stats.mqtt.last_error_code = -2;
  stats.mqtt.last_tls_err = -3;
  stats.mqtt.last_tls_stack_err = -4;
  stats.mqtt.last_tls_cert_verify_flags = 5;
  stats.mqtt.last_sock_errno = -6;
  stats.mqtt.last_connect_return_code = 7;
  stats.mqtt.last_error_at_ms = 9000;
  stats.mqtt.last_error_age_ms = 1000;
  stats.mqtt.heap_inactive = true;
  stats.mqtt.effective_transport = "tls";
  stats.mqtt.tls_live = 2;
  stats.mqtt.tls_cap = 4;
  stats.mqtt.broker_uri = "mqtts://broker.example:8883/path?x=1&y=2";
  stats.mqtt.broker_uri_valid = true;
  stats.mqtt.broker_username = "user\"name";
  stats.mqtt.reconnect_attempts_1h = 17;
  stats.mqtt.status_publishes = 18;
  stats.mqtt.packet_publishes = 19;
  stats.mqtt.session_status_publishes = 20;
  stats.mqtt.session_packet_publishes = 21;
  stats.mqtt.publish_failures = 22;
  stats.mqtt.publish_queue_depth = 2;
  stats.mqtt.publish_queue_cap = 8;
  stats.mqtt.publish_queue_drops = 23;
  stats.mqtt.publish_queue_byte_drops = 24;
  stats.mqtt.publish_queue_bytes = 2500;
  stats.mqtt.publish_queue_byte_cap = 8192;
  stats.mqtt.publish_queue_total_bytes = 2600;
  stats.mqtt.publish_queue_total_byte_cap = 24576;
  stats.mqtt.publish_outbox_size = -1;
  stats.mqtt.publish_outbox_cap = 32768;
  stats.mqtt.publish_outbox_drops = 25;
  stats.mqtt.connected = true;
  stats.mqtt.uptime_ms = 2700;
  stats.mqtt.neighbor_interval_s = 60;
  stats.mqtt.last_offline_epoch = 1700000001;
  return stats;
}

bool appendStats(CheckedBufferBuilder &builder,
                 int /*broker_idx*/,
                 const void *context) {
  if (context == nullptr) return false;
  return mqtt_payload::appendStatusStatsPayload(
      builder, *static_cast<const StatusStatsInput *>(context));
}

mqtt_payload::StatusPayloadInput fullStatus(const StatusStatsInput &stats) {
  mqtt_payload::StatusPayloadInput input = {};
  input.status = "on\"line";
  input.origin = "node\\name";
  input.origin_id = kOriginId;
  input.model = "Heltec V3";
  input.firmware_version = "1.0.0";
  input.radio = "SX1262 869.525/125/7/5";
  input.client_version = "mesh\nclient";
  input.time_synced = true;
  input.timestamp = "2026-09-13T16:20:30Z";
  input.broker_idx = 0;
  input.stats_appender = &appendStats;
  input.stats_context = &stats;
  return input;
}

mqtt_payload::PacketPayloadInput validPacket() {
  static const uint8_t raw[] = {0x01, 0xA0, 0xFF};
  static const uint8_t hash[] = {0xDE, 0xAD, 0xBE, 0xEF,
                                 0x01, 0x23, 0x45, 0x67};
  mqtt_payload::PacketPayloadInput input = {};
  input.direction = "rx";
  input.origin = "relay";
  input.origin_id = kOriginId;
  input.time_synced = true;
  input.timestamp = "2026-09-13T16:20:30Z";
  input.time = "16:20:30";
  input.date = "13/9/2026";
  input.len = 36;
  input.packet_type = 7;
  input.route_direct = true;
  input.packet_payload_len = 3;
  input.raw = raw;
  input.raw_len = sizeof(raw);
  input.include_radio_metrics = true;
  input.snr = -4;
  input.rssi = -110;
  input.score = 987;
  input.duration_ms = 42;
  input.hash = hash;
  input.hash_len = sizeof(hash);
  input.include_path = true;
  input.path = "AA -> BB";
  input.max_packet_payload = 184;
  input.encoded_path_len = 0;
  input.has_transport_codes = false;
  input.max_path_bytes = 64;
  input.max_raw_bytes = 255;
  return input;
}

TEST(MqttPayloadBuilders, StatusStatsIncludesDiagnosticsAndEscapesStrings) {
  const StatusStatsInput stats = fullStats();
  char buffer[4096];
  CheckedBufferBuilder builder(buffer, sizeof(buffer));

  ASSERT_TRUE(mqtt_payload::appendStatusStatsPayload(builder, stats));
  ASSERT_TRUE(builder.ok());
  EXPECT_EQ(builder.length(), std::strlen(buffer));

  const std::string payload(buffer, builder.length());
  EXPECT_NE(payload.find("\"reset_reason\":\"rst\\\"reason\""),
            std::string::npos);
  EXPECT_NE(payload.find("\"wifi_ssid\":\"lab\\\"ssid\""),
            std::string::npos);
  EXPECT_NE(payload.find("\"broker_username\":\"user\\\"name\""),
            std::string::npos);
  EXPECT_NE(payload.find("\"clock_trusted\":true"), std::string::npos);
  EXPECT_NE(payload.find("\"build_failures\":8"), std::string::npos);
  EXPECT_NE(payload.find("\"serialize_preflight_drops\":10"),
            std::string::npos);
  EXPECT_NE(payload.find("\"heap_internal_free\":270000"),
            std::string::npos);
  EXPECT_NE(payload.find("\"heap_psram_largest_block\":240000"),
            std::string::npos);

  const char *required_keys[] = {
      "clock_trusted", "ntp_sync_source", "ntp_sync_validated",
      "ntp_sync_fallback", "ntp_validation_mode", "ntp_attempt_generation",
      "ntp_last_attempt_age_ms", "build_failures", "serialize_preflight_drops",
      "heap_internal_free", "heap_internal_largest_block", "heap_psram_free",
      "heap_psram_largest_block", "last_tls_err", "last_tls_stack_err",
      "last_tls_cert_verify_flags", "effective_transport", "tls_live", "tls_cap",
      "publish_queue_byte_drops", "publish_queue_bytes", "publish_queue_byte_cap",
      "publish_queue_total_bytes", "publish_queue_total_byte_cap",
      "publish_outbox_size", "publish_outbox_cap"};
  for (const char *key : required_keys) {
    EXPECT_NE(payload.find(std::string("\"") + key + "\":"),
              std::string::npos)
        << "missing diagnostic key: " << key;
  }
}

TEST(MqttPayloadBuilders, StatusOmitsOptionalStatusMqttAndUsesNullClock) {
  StatusStatsInput stats = fullStats();
  stats.mqtt.present = false;
  stats.wifi_connected = false;
  stats.wifi_ssid = "";

  mqtt_payload::StatusPayloadInput input = fullStatus(stats);
  input.status = nullptr;
  input.time_synced = false;
  input.timestamp = nullptr;

  char buffer[4096];
  size_t payload_len = 777;
  ASSERT_TRUE(mqtt_payload::buildStatusPayload(
      buffer, sizeof(buffer), input, payload_len));
  const std::string payload(buffer, payload_len);
  EXPECT_EQ(payload.find("\"status\":"), std::string::npos);
  EXPECT_NE(payload.find("\"timestamp\":null"), std::string::npos);
  EXPECT_EQ(payload.find("\"mqtt\":"), std::string::npos);
  EXPECT_NE(payload.find("\"wifi_rssi\":null"), std::string::npos);
}

TEST(MqttPayloadBuilders, StatusRejectsInvalidStatsAndTruncation) {
  StatusStatsInput stats = fullStats();
  mqtt_payload::StatusPayloadInput input = fullStatus(stats);

  stats.mesh_stats = "{}";
  stats.mesh_stats_len = 2;
  char buffer[4096];
  size_t payload_len = 1;
  EXPECT_FALSE(mqtt_payload::buildStatusPayload(
      buffer, sizeof(buffer), input, payload_len));
  EXPECT_EQ(payload_len, 0u);

  stats = fullStats();
  input = fullStatus(stats);
  char reference[4096];
  size_t reference_len = 0;
  ASSERT_TRUE(mqtt_payload::buildStatusPayload(
      reference, sizeof(reference), input, reference_len));
  ASSERT_GT(reference_len, 1u);

  std::vector<char> short_buffer(reference_len, 'X');
  payload_len = 999;
  EXPECT_FALSE(mqtt_payload::buildStatusPayload(
      short_buffer.data(), short_buffer.size(), input, payload_len));
  EXPECT_EQ(payload_len, 0u);
  EXPECT_LT(std::strlen(short_buffer.data()), short_buffer.size());
  EXPECT_FALSE(mqtt_payload::buildStatusPayload(nullptr, 0, input, payload_len));
  EXPECT_EQ(payload_len, 0u);
}

TEST(MqttPayloadBuilders, PacketBuilderPreservesWireShapeAtFullTelemetry) {
  mqtt_payload::PacketPayloadInput input = validPacket();
  char buffer[4096];
  size_t payload_len = 0;
  ASSERT_TRUE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));

  std::string expected =
      "{\"origin\":\"relay\",\"origin_id\":\"";
  expected += kOriginId;
  expected +=
      "\",\"timestamp\":\"2026-09-13T16:20:30Z\",\"type\":\"PACKET\","
      "\"direction\":\"rx\",\"time\":\"16:20:30\",\"date\":\"13/9/2026\","
      "\"len\":\"36\",\"packet_type\":\"7\",\"route\":\"D\","
      "\"payload_len\":\"3\",\"raw\":\"01A0FF\",\"SNR\":\"-4\","
      "\"RSSI\":\"-110\",\"score\":\"987\",\"duration\":\"42\","
      "\"hash\":\"DEADBEEF01234567\",\"path\":\"AA -> BB\"}";
  EXPECT_EQ(std::string(buffer, payload_len), expected);
}

TEST(MqttPayloadBuilders, PacketBuilderOmitsUnsyncedAndNonRadioFields) {
  mqtt_payload::PacketPayloadInput input = validPacket();
  input.time_synced = false;
  input.timestamp = nullptr;
  input.time = nullptr;
  input.date = nullptr;
  input.route_direct = false;
  input.include_radio_metrics = false;
  input.include_path = true;

  char buffer[4096];
  size_t payload_len = 0;
  ASSERT_TRUE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));
  const std::string payload(buffer, payload_len);
  EXPECT_NE(payload.find("\"timestamp\":null"), std::string::npos);
  EXPECT_NE(payload.find("\"time\":null,\"date\":null"), std::string::npos);
  EXPECT_NE(payload.find("\"route\":\"F\""), std::string::npos);
  EXPECT_EQ(payload.find("\"SNR\":"), std::string::npos);
  EXPECT_EQ(payload.find("\"path\":"), std::string::npos);
}

TEST(MqttPayloadBuilders, PacketPreflightAcceptsExactRawBoundaryAndRejectsEdges) {
  mqtt_payload::PacketPayloadInput input = validPacket();
  std::vector<uint8_t> raw(254, 0xA5);
  input.raw = raw.data();
  input.raw_len = raw.size();
  input.packet_payload_len = 184;
  input.encoded_path_len = (1U << 6) | 32U;  // 32 two-byte hashes = 64 bytes.
  input.has_transport_codes = true;

  char buffer[4096];
  size_t payload_len = 0;
  ASSERT_TRUE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));

  input.max_raw_bytes = 253;
  payload_len = 123;
  EXPECT_FALSE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));
  EXPECT_EQ(payload_len, 0u);

  input = validPacket();
  input.packet_payload_len = input.max_packet_payload + 1;
  EXPECT_FALSE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));
  input = validPacket();
  input.encoded_path_len = 0xC0;
  EXPECT_FALSE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));
  input = validPacket();
  input.raw_len = input.max_raw_bytes + 1;
  EXPECT_FALSE(mqtt_payload::buildPacketPayload(
      buffer, sizeof(buffer), input, payload_len));
}

TEST(MqttPayloadBuilders, PacketTruncationLeavesNoUsablePartialPayload) {
  mqtt_payload::PacketPayloadInput input = validPacket();
  char reference[4096];
  size_t reference_len = 0;
  ASSERT_TRUE(mqtt_payload::buildPacketPayload(
      reference, sizeof(reference), input, reference_len));
  ASSERT_GT(reference_len, 1u);

  std::vector<char> short_buffer(reference_len, 'X');
  size_t payload_len = 1;
  EXPECT_FALSE(mqtt_payload::buildPacketPayload(
      short_buffer.data(), short_buffer.size(), input, payload_len));
  EXPECT_EQ(payload_len, 0u);
  EXPECT_LT(std::strlen(short_buffer.data()), short_buffer.size());
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
