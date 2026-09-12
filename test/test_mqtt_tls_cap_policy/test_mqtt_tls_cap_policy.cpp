// Native unit tests for effective live MQTT transport accounting.

#include <gtest/gtest.h>

#include "helpers/MqttTlsCapPolicy.h"

using mqtt_tls_cap::ClientState;
using mqtt_tls_cap::Slot;
using mqtt_tls_cap::Transport;
using mqtt_tls_cap::canStart;
using mqtt_tls_cap::liveTlsCount;
using mqtt_tls_cap::recordClientConnected;
using mqtt_tls_cap::recordClientCreated;
using mqtt_tls_cap::recordClientDestroyed;
using mqtt_tls_cap::recordClientDisconnected;
using mqtt_tls_cap::transportForUri;

namespace {

Slot emptySlot() {
  Slot slot = {};
  slot.configured_enabled = false;
  slot.client_state = ClientState::None;
  slot.effective_transport = Transport::Unknown;
  return slot;
}

}  // namespace

TEST(MqttTlsCapPolicy, ClassifiesSupportedSchemes) {
  EXPECT_EQ(transportForUri("mqtt://broker"), Transport::Plain);
  EXPECT_EQ(transportForUri("ws://broker"), Transport::Plain);
  EXPECT_EQ(transportForUri("mqtts://broker"), Transport::Tls);
  EXPECT_EQ(transportForUri("wss://broker"), Transport::Tls);
  EXPECT_EQ(transportForUri("custom://broker"), Transport::Unknown);
}

TEST(MqttTlsCapPolicy, DisabledConnectedClientStillConsumesTlsBudget) {
  Slot slots[2] = {emptySlot(), emptySlot()};
  slots[0].configured_enabled = false;
  recordClientCreated(slots[0], Transport::Tls);
  recordClientConnected(slots[0]);

  EXPECT_EQ(liveTlsCount(slots, 2), 1u);
  EXPECT_FALSE(canStart(Transport::Tls, slots, 2, false, 1, 6));
}

TEST(MqttTlsCapPolicy, ConnectingAndDisconnectedClientsStillConsumeBudget) {
  Slot slots[3] = {emptySlot(), emptySlot(), emptySlot()};
  recordClientCreated(slots[0], Transport::Tls);
  recordClientCreated(slots[1], Transport::Tls);
  recordClientDisconnected(slots[1]);

  EXPECT_EQ(slots[0].client_state, ClientState::Connecting);
  EXPECT_EQ(slots[1].client_state, ClientState::Disconnected);
  EXPECT_EQ(liveTlsCount(slots, 3), 2u);
  EXPECT_FALSE(canStart(Transport::Tls, slots, 3, false, 2, 6));
}

TEST(MqttTlsCapPolicy, StoredUriEditDoesNotReclassifyLiveTransport) {
  Slot slots[2] = {emptySlot(), emptySlot()};
  slots[0].configured_enabled = true;
  recordClientCreated(slots[0], Transport::Tls);
  recordClientConnected(slots[0]);

  // The setting can now say mqtt://, but the allocated SDK client remains TLS.
  slots[0].configured_enabled = true;
  EXPECT_EQ(transportForUri("mqtt://broker"), Transport::Plain);
  EXPECT_EQ(liveTlsCount(slots, 2), 1u);
  EXPECT_FALSE(canStart(Transport::Tls, slots, 2, false, 1, 6));

  recordClientDestroyed(slots[0]);
  EXPECT_TRUE(canStart(Transport::Tls, slots, 2, false, 1, 6));
}

TEST(MqttTlsCapPolicy, FailedPsramInitializationUsesSmallerRuntimeCap) {
  Slot slots[6] = {emptySlot(), emptySlot(), emptySlot(), emptySlot(), emptySlot(), emptySlot()};
  recordClientCreated(slots[0], Transport::Tls);
  recordClientCreated(slots[1], Transport::Tls);

  EXPECT_FALSE(canStart(Transport::Tls, slots, 6, false, 2, 6));
  EXPECT_TRUE(canStart(Transport::Tls, slots, 6, true, 2, 6));
}

TEST(MqttTlsCapPolicy, PlaintextDoesNotConsumeTlsBudget) {
  Slot slots[3] = {emptySlot(), emptySlot(), emptySlot()};
  recordClientCreated(slots[0], Transport::Tls);
  recordClientCreated(slots[1], Transport::Plain);
  recordClientConnected(slots[1]);

  EXPECT_EQ(liveTlsCount(slots, 3), 1u);
  EXPECT_TRUE(canStart(Transport::Plain, slots, 3, false, 1, 6));
}

TEST(MqttTlsCapPolicy, UnknownTransportIsChargedConservatively) {
  Slot slots[2] = {emptySlot(), emptySlot()};
  recordClientCreated(slots[0], Transport::Unknown);
  EXPECT_EQ(liveTlsCount(slots, 2), 1u);
  EXPECT_FALSE(canStart(Transport::Tls, slots, 2, false, 1, 6));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
