#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "helpers/MqttWirePreflight.h"

namespace {
constexpr size_t kMaxPayload = 184;
constexpr size_t kMaxPath = 64;
constexpr size_t kMaxRaw = 255;
}

TEST(MqttWirePreflight, ExactLargestPacketIsAccepted) {
  // Two-byte path hashes: 32 * 2 = 64 bytes. With transport codes and the
  // maximum payload this is the largest representable current packet (254 B).
  const uint8_t encoded_path = (uint8_t)((2 - 1) << 6) | 32;
  mqtt_wire::Preflight result = mqtt_wire::validate(
      kMaxPayload, encoded_path, true, kMaxPayload, kMaxPath, kMaxRaw);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.path_bytes, 64u);
  EXPECT_EQ(result.raw_bytes, 254u);

  // The exact computed boundary is accepted; one byte less is rejected.
  EXPECT_TRUE(mqtt_wire::validate(
      kMaxPayload, encoded_path, true, kMaxPayload, kMaxPath, result.raw_bytes).ok());
  EXPECT_EQ(mqtt_wire::validate(
                kMaxPayload, encoded_path, true, kMaxPayload, kMaxPath,
                result.raw_bytes - 1).failure,
            mqtt_wire::Failure::RawLength);
}

TEST(MqttWirePreflight, OversizePayloadIsRejectedBeforeSerialization) {
  mqtt_wire::Preflight result = mqtt_wire::validate(
      kMaxPayload + 1, 0, false, kMaxPayload, kMaxPath, kMaxRaw);
  EXPECT_EQ(result.failure, mqtt_wire::Failure::PayloadLength);
}

TEST(MqttWirePreflight, PathLengthEdgesAreCheckedFromEncoding) {
  const uint8_t valid_three_byte_path = (uint8_t)((3 - 1) << 6) | 21;   // 63 B
  const uint8_t too_long_three_byte_path = (uint8_t)((3 - 1) << 6) | 22; // 66 B
  EXPECT_TRUE(mqtt_wire::validate(
      0, valid_three_byte_path, false, kMaxPayload, kMaxPath, kMaxRaw).ok());
  EXPECT_EQ(mqtt_wire::validate(
                0, too_long_three_byte_path, false, kMaxPayload, kMaxPath, kMaxRaw).failure,
            mqtt_wire::Failure::PathLength);
}

TEST(MqttWirePreflight, ReservedAndWidePathEncodingsAreRejected) {
  EXPECT_EQ(mqtt_wire::validate(
                0, 0xC0, false, kMaxPayload, kMaxPath, kMaxRaw).failure,
            mqtt_wire::Failure::PathEncoding);
  EXPECT_EQ(mqtt_wire::validate(
                0, 256, false, kMaxPayload, kMaxPath, kMaxRaw).failure,
            mqtt_wire::Failure::PathEncoding);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
