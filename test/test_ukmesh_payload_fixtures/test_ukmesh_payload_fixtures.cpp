// Golden ukmesh payload contract fixtures — structural regression checks.
//
// The frozen samples in test/fixtures/ukmesh/ define the ukmesh payload
// contract (status / packets / neighbors families). These tests assert the
// fixtures stay well-formed and contain the expected key sets, so an
// accidental edit or a port that changes the payload shape trips CI.
//
// Updating the contract is a deliberate act: re-capture a sample, sanitize
// it, replace the fixture, and update the expected key lists below in the
// same change.

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

std::string readFile(const std::string &path, bool &ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { ok = false; return {}; }
  std::ostringstream ss;
  ss << f.rdbuf();
  ok = true;
  return ss.str();
}

std::string fixtureText(const char *name) {
  const std::string file = __FILE__;
  const std::string marker = "test/test_ukmesh_payload_fixtures";
  std::string root;
  auto pos = file.find(marker);
  if (pos != std::string::npos) root = file.substr(0, pos);
  const std::string candidates[] = {
      root + "test/fixtures/ukmesh/" + name,
      std::string("test/fixtures/ukmesh/") + name,
      std::string("../test/fixtures/ukmesh/") + name,
      std::string("../../test/fixtures/ukmesh/") + name,
  };
  for (const auto &c : candidates) {
    bool ok = false;
    std::string t = readFile(c, ok);
    if (ok) return t;
  }
  ADD_FAILURE() << "fixture not found: " << name << " (__FILE__=" << file << ")";
  return {};
}

bool containsKey(const std::string &json, const std::string &key) {
  return json.find("\"" + key + "\":") != std::string::npos;
}

// First character class of the value for a key: s=tring, d=digit, b=bool,
// n=null, o=object, ?=missing.
char valueKind(const std::string &json, const std::string &key) {
  auto p = json.find("\"" + key + "\":");
  if (p == std::string::npos) return '?';
  size_t i = p + key.size() + 3;
  while (i < json.size() && json[i] == ' ') i++;
  if (i >= json.size()) return '?';
  char c = json[i];
  if (c == '"') return 's';
  if (c == 't' || c == 'f') return 'b';
  if (c == 'n') return 'n';
  if (c == '{') return 'o';
  if (c == '-' || std::isdigit((static_cast<unsigned char>(c)))) return 'd';
  return '?';
}

// Balanced-braces sanity (catches truncation / accidental splicing).
void expectBalanced(const std::string &json) {
  int curly = 0, square = 0;
  for (char c : json) {
    if (c == '{') curly++;
    else if (c == '}') curly--;
    else if (c == '[') square++;
    else if (c == ']') square--;
  }
  EXPECT_EQ(curly, 0);
  EXPECT_EQ(square, 0);
}

bool isHex(const std::string &s, size_t expect_len) {
  if (s.size() != expect_len) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// Extract a string value for key (naive but sufficient for flat fixtures).
std::string stringValue(const std::string &json, const std::string &key) {
  auto p = json.find("\"" + key + "\":\"");
  if (p == std::string::npos) return {};
  size_t start = p + key.size() + 4;
  size_t end = json.find('"', start);
  if (end == std::string::npos) return {};
  return json.substr(start, end - start);
}

// AUTO-GENERATED key lists are baked from the frozen fixtures (C6).
const char *kStatusTopKeys[] = {
    "status",
    "origin",
    "origin_id",
    "model",
    "firmware_version",
    "radio",
    "client_version",
    "timestamp",
    "stats",
};

const char *kStatusStatsKeys[] = {
    "uptime_ms",
    "boot_count",
    "reset_reason",
    "ntp_synced",
    "ntp_sync_age_ms",
    "boot_epoch",
    "max_loop_ms",
    "max_loop_at_ms",
    "loop_iterations",
    "wifi_reconnect_attempts",
    "rx_publish_calls",
    "tx_publish_calls",
    "tx_fail_publish_calls",
    "publish_skipped_no_connection",
    "forward_successes",
    "forward_successes_flood",
    "forward_successes_direct",
    "forward_failures",
    "tx_queue_depth",
    "tx_queue_depth_peak",
    "heap_free",
    "heap_min_free",
    "heap_min_seen_since_boot",
    "wifi_connected",
    "wifi_uptime_ms",
    "wifi_rssi",
    "wifi_ssid",
    "nodes_heard_24h",
    "last_rx_rssi",
    "last_rx_snr",
    "tx_power_dbm",
    "last_tx_fail_reason",
    "config_version",
    "config_crc32",
    "fs_free_bytes",
    "fs_total_bytes",
    "nvs_free_entries",
    "power_source",
    "solar_mv",
    "board_temp_c",
    "channel_id",
    "git_commit",
    "idle_pct_core0",
    "idle_pct_core1",
    "battery_mv",
    "uptime_secs",
    "tx_air_secs",
    "rx_air_secs",
    "channel_utilization",
    "air_util_tx",
    "air_util_rx",
    "mqtt",
};

const char *kStatusMqttKeys[] = {
    "broker_index",
    "connect_attempts",
    "connect_start_failures",
    "connect_events",
    "disconnect_events",
    "error_events",
    "reconnect_rung",
    "reconnect_breaker",
    "reconnect_next_in_ms",
    "last_error_type",
    "last_error_code",
    "heap_inactive",
    "broker_uri",
    "broker_username",
    "reconnect_attempts_1h",
    "status_publishes",
    "packet_publishes",
    "session_status_publishes",
    "session_packet_publishes",
    "publish_failures",
    "publish_queue_depth",
    "publish_queue_drops",
    "publish_outbox_drops",
    "connected",
    "uptime_ms",
    "neighbor_interval_s",
    "last_offline_epoch",
};

const char *kPacketKeys[] = {
    "origin",
    "origin_id",
    "timestamp",
    "type",
    "direction",
    "time",
    "date",
    "len",
    "packet_type",
    "route",
    "payload_len",
    "raw",
    "hash",
};

TEST(UkmeshPayloadFixtures, StatusTopLevelContract) {
  const std::string j = fixtureText("status-online.sample.json");
  ASSERT_FALSE(j.empty());
  expectBalanced(j);
  for (const char *k : kStatusTopKeys) {
    EXPECT_TRUE(containsKey(j, k)) << "missing top-level key: " << k;
  }
  EXPECT_EQ(stringValue(j, "origin_id").size(), 64u);
  EXPECT_TRUE(isHex(stringValue(j, "origin_id"), 64));
  const std::string status = stringValue(j, "status");
  EXPECT_TRUE(status == "online" || status == "offline") << status;
  EXPECT_EQ(valueKind(j, "stats"), 'o');
}

TEST(UkmeshPayloadFixtures, StatusStatsContract) {
  const std::string j = fixtureText("status-online.sample.json");
  ASSERT_FALSE(j.empty());
  for (const char *k : kStatusStatsKeys) {
    EXPECT_TRUE(containsKey(j, k)) << "missing stats key: " << k;
  }
  for (const char *k : kStatusMqttKeys) {
    EXPECT_TRUE(containsKey(j, k)) << "missing stats.mqtt key: " << k;
  }
  // Sanitization guard: the frozen fixture must never carry a real SSID.
  EXPECT_TRUE(containsKey(j, "wifi_ssid"));
  EXPECT_EQ(stringValue(j, "wifi_ssid"), "REDACTED");
  EXPECT_EQ(valueKind(j, "uptime_ms"), 'd');
  EXPECT_EQ(valueKind(j, "heap_free"), 'd');
}

TEST(UkmeshPayloadFixtures, PacketContract) {
  const std::string j = fixtureText("packet-tx.sample.json");
  ASSERT_FALSE(j.empty());
  expectBalanced(j);
  for (const char *k : kPacketKeys) {
    EXPECT_TRUE(containsKey(j, k)) << "missing packet key: " << k;
  }
  EXPECT_EQ(stringValue(j, "type"), "PACKET");
  const std::string dir = stringValue(j, "direction");
  EXPECT_TRUE(dir == "tx" || dir == "rx") << dir;
  EXPECT_TRUE(isHex(stringValue(j, "origin_id"), 64));
  EXPECT_TRUE(isHex(stringValue(j, "hash"), 16));
  // Numbers in this payload family are string-encoded on the wire.
  EXPECT_EQ(stringValue(j, "len"), "36");
  EXPECT_EQ(stringValue(j, "payload_len"), "20");
  const std::string raw = stringValue(j, "raw");
  EXPECT_FALSE(raw.empty());
  for (char c : raw) {
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
  }
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
