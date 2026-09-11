// Native tests for the MQTT prefs codec + legacy v1/v2/v3 -> v4 migration.
//
// Binary fixtures in test/fixtures/prefs/ pin the on-disk layouts and the
// exact migration semantics (B1(d)). The layouts are also static_asserted in
// helpers/MqttPrefsCodec.h; these tests exercise the runtime behaviour the
// store relies on: classification, field mapping, bounded copies, CRC and
// serialization round-trips.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "helpers/MqttPrefsCodec.h"

using namespace mqtt_prefs;

namespace {

std::string readFileBytes(const std::string &path, bool &ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    ok = false;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  ok = true;
  return ss.str();
}

std::vector<uint8_t> fixtureBytes(const char *name) {
  const std::string file = __FILE__;
  const std::string marker = "test/test_mqtt_prefs_codec";
  std::string root;
  auto pos = file.find(marker);
  if (pos != std::string::npos) root = file.substr(0, pos);
  const std::string candidates[] = {
      root + "test/fixtures/prefs/" + name,
      std::string("test/fixtures/prefs/") + name,
      std::string("../test/fixtures/prefs/") + name,
      std::string("../../test/fixtures/prefs/") + name,
  };
  for (const auto &c : candidates) {
    bool ok = false;
    std::string t = readFileBytes(c, ok);
    if (ok) return std::vector<uint8_t>(t.begin(), t.end());
  }
  ADD_FAILURE() << "fixture not found: " << name << " (__FILE__=" << file << ")";
  return {};
}

std::string cstr(const char *p, size_t n) {
  size_t len = 0;
  while (len < n && p[len] != '\0') len++;
  return std::string(p, len);
}

}  // namespace

TEST(MqttPrefsCodec, LayoutsArePinned) {
  EXPECT_EQ(sizeof(Header), 8u);
  EXPECT_EQ(sizeof(SharedConfig), 256u);
  EXPECT_EQ(sizeof(BrokerConfig), 536u);
  EXPECT_EQ(sizeof(LegacyRuntimeConfigV1), 561u);
  EXPECT_EQ(sizeof(LegacyFileV1), 572u);
  EXPECT_EQ(sizeof(LegacyBrokerConfigV2), 308u);
  EXPECT_EQ(sizeof(LegacyFileV2), 2112u);
  EXPECT_EQ(sizeof(LegacyBrokerConfigV3), 532u);
  EXPECT_EQ(sizeof(LegacyFileV3), 3456u);
  EXPECT_EQ(sizeof(FileV4), 3480u);
}

TEST(MqttPrefsCodec, ClassifyStoredFiles) {
  auto k = [](const char *name) {
    auto b = fixtureBytes(name);
    return classify(b.data(), b.size());
  };
  EXPECT_EQ(k("mqtt-v1.bin"), Kind::V1);
  EXPECT_EQ(k("mqtt-v2.bin"), Kind::V2);
  EXPECT_EQ(k("mqtt-v3.bin"), Kind::V3);
  EXPECT_EQ(k("mqtt-v4.bin"), Kind::V4);
  EXPECT_EQ(k("mqtt-unknown-version.bin"), Kind::Unrecognized);
  EXPECT_EQ(k("mqtt-bad-magic.bin"), Kind::BadMagic);

  const uint8_t tiny[4] = {0x54, 0x54, 0x51, 0x4D};
  EXPECT_EQ(classify(tiny, sizeof(tiny)), Kind::TooSmall);
}

TEST(MqttPrefsCodec, V1MigrationMapsAllFields) {
  auto b = fixtureBytes("mqtt-v1.bin");
  ASSERT_EQ(b.size(), sizeof(LegacyFileV1));
  const LegacyFileV1 *v1 = reinterpret_cast<const LegacyFileV1 *>(b.data());

  SharedConfig shared{};
  mapSharedFromV1(*v1, shared);
  EXPECT_EQ(cstr(shared.wifi_ssid, sizeof(shared.wifi_ssid)), "OldNet");
  EXPECT_EQ(cstr(shared.wifi_pwd, sizeof(shared.wifi_pwd)), "oldpw");
  EXPECT_EQ(cstr(shared.model, sizeof(shared.model)), "Heltec V3");
  EXPECT_EQ(cstr(shared.client_version, sizeof(shared.client_version)), "meshcore-mqtt/v1.4.0");

  BrokerConfig dst{};
  mapBrokerFromV1(v1->config, dst);
  EXPECT_EQ(cstr(dst.uri, sizeof(dst.uri)), "wss://old.example:443/");
  EXPECT_EQ(cstr(dst.username, sizeof(dst.username)), "olduser");
  EXPECT_EQ(cstr(dst.password, sizeof(dst.password)), "oldpass");
  EXPECT_EQ(cstr(dst.topic_root, sizeof(dst.topic_root)), "meshcore-test/TST");
  EXPECT_EQ(cstr(dst.iata, sizeof(dst.iata)), "OLD");
  EXPECT_EQ(dst.retain_status, 1);
  EXPECT_EQ(dst.enabled, 1);  // v1 broker becomes enabled
  EXPECT_EQ(dst.neighbor_interval_secs, 0u);  // store sanitizes the default in
}

TEST(MqttPrefsCodec, V2MigrationMapsAllBrokers) {
  auto b = fixtureBytes("mqtt-v2.bin");
  ASSERT_EQ(b.size(), sizeof(LegacyFileV2));
  const LegacyFileV2 *v2 = reinterpret_cast<const LegacyFileV2 *>(b.data());

  EXPECT_EQ(cstr(v2->shared.wifi_ssid, sizeof(v2->shared.wifi_ssid)), "V2Net");
  EXPECT_EQ(v2->broker_count, 2);

  BrokerConfig b0{}, b1{};
  mapBrokerFromV2(v2->brokers[0], b0);
  mapBrokerFromV2(v2->brokers[1], b1);
  EXPECT_EQ(cstr(b0.uri, 128), "wss://two.example:443/");
  EXPECT_EQ(cstr(b0.topic_root, 256), "meshcore-test/TST");
  EXPECT_EQ(cstr(b0.iata, 16), "TST");
  EXPECT_EQ(b0.enabled, 1);
  EXPECT_EQ(cstr(b1.uri, 128), "mqtt://one.example:1883");
  EXPECT_EQ(b1.enabled, 0);
  EXPECT_EQ(b1.retain_status, 0);
  EXPECT_EQ(v2->brokers[2].uri[0], '\0');
}

TEST(MqttPrefsCodec, V3MigrationSetsIntervalDefault) {
  auto b = fixtureBytes("mqtt-v3.bin");
  ASSERT_EQ(b.size(), sizeof(LegacyFileV3));
  const LegacyFileV3 *v3 = reinterpret_cast<const LegacyFileV3 *>(b.data());

  BrokerConfig dst{};
  mapBrokerFromV3(v3->brokers[0], dst, 900u);
  EXPECT_EQ(cstr(dst.uri, 128), "wss://three.example:443/");
  EXPECT_EQ(cstr(dst.topic_root, 256), "meshcore-test/MME");
  EXPECT_EQ(cstr(dst.iata, 16), "MME");
  EXPECT_EQ(dst.enabled, 1);
  EXPECT_EQ(dst.neighbor_interval_secs, 900u);
}

TEST(MqttPrefsCodec, V4FixtureParsesFieldByField) {
  auto b = fixtureBytes("mqtt-v4.bin");
  ASSERT_EQ(b.size(), sizeof(FileV4));
  FileV4 img;
  std::memcpy(&img, b.data(), sizeof(img));

  EXPECT_EQ(img.header.magic, kMagic);
  EXPECT_EQ(img.header.version, 4);
  EXPECT_EQ(img.header.broker_count, 1);
  EXPECT_EQ(cstr(img.shared.wifi_ssid, sizeof(img.shared.wifi_ssid)), "V4Net");
  EXPECT_EQ(cstr(img.brokers[0].uri, 128), "wss://mqtt.ukmesh.com:443/");
  EXPECT_EQ(cstr(img.brokers[0].username, 64), "hermes-test");
  EXPECT_EQ(cstr(img.brokers[0].topic_root, 256), "meshcore-test");
  EXPECT_EQ(cstr(img.brokers[0].iata, 16), "TST");
  EXPECT_EQ(img.brokers[0].neighbor_interval_secs, 900u);
  EXPECT_EQ(img.brokers[1].uri[0], '\0');
}

TEST(MqttPrefsCodec, CopyStrSemantics) {
  char dst[8];
  copyStr(dst, "1234567890", sizeof(dst));
  EXPECT_EQ(std::string(dst), "1234567");  // buf_sz-1 max, single terminator

  char exact[8];
  copyStr(exact, "1234567", sizeof(exact));
  EXPECT_EQ(std::string(exact), "1234567");

  char empty[8];
  copyStr(empty, "", sizeof(empty));
  EXPECT_EQ(std::string(empty), "");
}

TEST(MqttPrefsCodec, Crc32KnownVector) {
  const char *s = "123456789";
  uint32_t crc = 0xFFFFFFFFUL;
  crc32Update(crc, reinterpret_cast<const uint8_t *>(s), 9);
  EXPECT_EQ(crc32Finish(crc), 0xCBF43926UL);
}

TEST(MqttPrefsCodec, SerializeRoundTrip) {
  SharedConfig shared{};
  copyStr(shared.wifi_ssid, "RoundTrip", sizeof(shared.wifi_ssid));
  copyStr(shared.model, "Heltec V3", sizeof(shared.model));

  BrokerConfig brokers[kMaxBrokers] = {};
  copyStr(brokers[0].uri, "wss://round.example:443/", sizeof(brokers[0].uri));
  copyStr(brokers[0].topic_root, "meshcore-test", sizeof(brokers[0].topic_root));
  copyStr(brokers[0].iata, "TST", sizeof(brokers[0].iata));
  brokers[0].retain_status = 1;
  brokers[0].enabled = 1;
  brokers[0].neighbor_interval_secs = 900;

  uint8_t out[sizeof(FileV4)];
  serializeV4(1, shared, brokers, out);
  ASSERT_EQ(classify(out, sizeof(out)), Kind::V4);

  FileV4 img;
  std::memcpy(&img, out, sizeof(img));
  EXPECT_EQ(img.header.broker_count, 1);
  EXPECT_EQ(cstr(img.shared.wifi_ssid, 64), "RoundTrip");
  EXPECT_EQ(cstr(img.brokers[0].uri, 128), "wss://round.example:443/");
  EXPECT_EQ(img.brokers[0].neighbor_interval_secs, 900u);
  EXPECT_EQ(img.brokers[1].uri[0], '\0');
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
