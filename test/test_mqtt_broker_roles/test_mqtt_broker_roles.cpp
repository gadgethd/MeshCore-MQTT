#include <gtest/gtest.h>

#include "helpers/MqttBrokerRoles.h"

namespace {

using mqtt_broker_roles::BrokerEntry;
using mqtt_broker_roles::Role;
using mqtt_broker_roles::classify;

BrokerEntry entry(bool enabled, const char *root, const char *uri = "wss://mqtt.example/",
                  const char *iata = "MME") {
  return BrokerEntry{enabled, root, uri, iata};
}

void expectFull(const Role &role) {
  EXPECT_FALSE(role.status_only);
  EXPECT_FALSE(role.status_suppressed);
}

}  // namespace

TEST(MqttBrokerRoles, SinglePacketsEntryKeepsFullBehaviour) {
  const BrokerEntry entries[] = {entry(true, "meshcore/MME/node/packets")};
  Role roles[1] = {};

  classify(entries, 1, roles);

  expectFull(roles[0]);
}

TEST(MqttBrokerRoles, SameUriStatusSiblingSplitsFamilies) {
  const BrokerEntry entries[] = {
      entry(true, "meshcore/MME/node/packets"),
      entry(true, "meshcore/MME/node/status"),
  };
  Role roles[2] = {};

  classify(entries, 2, roles);

  EXPECT_FALSE(roles[0].status_only);
  EXPECT_TRUE(roles[0].status_suppressed);
  EXPECT_TRUE(roles[1].status_only);
  EXPECT_FALSE(roles[1].status_suppressed);
}

TEST(MqttBrokerRoles, DifferentUriStatusSiblingIsAnAdditionalMirror) {
  const BrokerEntry entries[] = {
      entry(true, "meshcore/MME/node/packets", "wss://primary.example/"),
      entry(true, "meshcore/MME/node/status", "wss://status.example/"),
  };
  Role roles[2] = {};

  classify(entries, 2, roles);

  expectFull(roles[0]);
  EXPECT_TRUE(roles[1].status_only);
  EXPECT_FALSE(roles[1].status_suppressed);
}

TEST(MqttBrokerRoles, LoneStatusEntryIsStatusOnly) {
  const BrokerEntry entries[] = {entry(true, "meshcore/MME/node/status")};
  Role roles[1] = {};

  classify(entries, 1, roles);

  EXPECT_TRUE(roles[0].status_only);
  EXPECT_FALSE(roles[0].status_suppressed);
}

TEST(MqttBrokerRoles, DisabledStatusSiblingDoesNotSuppressPrimary) {
  const BrokerEntry entries[] = {
      entry(true, "meshcore/MME/node/packets"),
      entry(false, "meshcore/MME/node/status"),
  };
  Role roles[2] = {};

  classify(entries, 2, roles);

  expectFull(roles[0]);
  expectFull(roles[1]);
}

TEST(MqttBrokerRoles, BareRootKeepsFullBehaviour) {
  const BrokerEntry entries[] = {entry(true, "meshcore/MME/node")};
  Role roles[1] = {};

  classify(entries, 1, roles);

  expectFull(roles[0]);
}

TEST(MqttBrokerRoles, FourSlotConfigurationSplitsBothPairs) {
  const BrokerEntry entries[] = {
      entry(true, "meshcore/MME/one/packets", "wss://one.example/", "MME"),
      entry(true, "meshcore/MME/one/status", "wss://one.example/", "MME"),
      entry(true, "meshcore/LAB/two/packets", "wss://two.example/", "LAB"),
      entry(true, "meshcore/LAB/two/status", "wss://two.example/", "LAB"),
  };
  Role roles[4] = {};

  classify(entries, 4, roles);

  EXPECT_TRUE(roles[0].status_suppressed);
  EXPECT_TRUE(roles[1].status_only);
  EXPECT_TRUE(roles[2].status_suppressed);
  EXPECT_TRUE(roles[3].status_only);
}

TEST(MqttBrokerRoles, TokenExpandedRootsAreClassifiedByEffectiveSuffix) {
  const BrokerEntry entries[] = {
      // meshcore/{IATA}/one/packets and .../status after expansion.
      entry(true, "meshcore/MME/one/packets", "wss://one.example/", "MME"),
      entry(true, "meshcore/MME/one/status", "wss://one.example/", "MME"),
      // meshcore/{PUBLIC_KEY}/two/packets and .../status after expansion.
      entry(true, "meshcore/ABCDEF/two/packets", "wss://two.example/", "MME"),
      entry(true, "meshcore/ABCDEF/two/status", "wss://two.example/", "MME"),
      // meshcore/<IATA>/three/packets and .../status after expansion.
      entry(true, "meshcore/LAB/three/packets", "wss://three.example/", "LAB"),
      entry(true, "meshcore/LAB/three/status", "wss://three.example/", "LAB"),
      // meshcore/<PUBLIC_KEY>/four/packets and .../status after expansion.
      entry(true, "meshcore/123456/four/packets", "wss://four.example/", "LAB"),
      entry(true, "meshcore/123456/four/status", "wss://four.example/", "LAB"),
  };
  Role roles[8] = {};

  classify(entries, 8, roles);

  EXPECT_TRUE(roles[0].status_suppressed);
  EXPECT_TRUE(roles[1].status_only);
  EXPECT_TRUE(roles[2].status_suppressed);
  EXPECT_TRUE(roles[3].status_only);
  EXPECT_TRUE(roles[4].status_suppressed);
  EXPECT_TRUE(roles[5].status_only);
  EXPECT_TRUE(roles[6].status_suppressed);
  EXPECT_TRUE(roles[7].status_only);
}

TEST(MqttBrokerRoles, SameUriWithDifferentIataDoesNotSuppress) {
  const BrokerEntry entries[] = {
      entry(true, "meshcore/MME/node/packets", "wss://mqtt.example/", "MME"),
      entry(true, "meshcore/LAB/node/status", "wss://mqtt.example/", "LAB"),
  };
  Role roles[2] = {};

  classify(entries, 2, roles);

  expectFull(roles[0]);
  EXPECT_TRUE(roles[1].status_only);
  EXPECT_FALSE(roles[1].status_suppressed);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
