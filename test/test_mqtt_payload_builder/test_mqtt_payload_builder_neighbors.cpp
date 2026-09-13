#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "helpers/MqttPayloadBuilders.h"

namespace {

using mqtt_payload::NeighborRecord;

struct NeighborFixture {
  NeighborRecord records[3];
  size_t count;
  size_t fail_index;
};

bool readNeighbors(const void *context, size_t index, NeighborRecord &record) {
  if (context == nullptr) return false;
  const NeighborFixture &fixture = *static_cast<const NeighborFixture *>(context);
  if (index == fixture.fail_index || index >= fixture.count) return false;
  record = fixture.records[index];
  return true;
}

TEST(MqttPayloadBuilders, NeighborsBuilderUsesVisitorAndWrapSafeAge) {
  static const uint8_t first_id[] = {0x00, 0xAB, 0x10, 0xFF};
  static const uint8_t second_id[] = {0x12, 0x34, 0x56, 0x78};
  NeighborFixture fixture = {
      {{first_id, sizeof(first_id), -90, 5.5f, 7000, true},
       {nullptr, 0, 0, 0.0f, 0, false},
       {second_id, sizeof(second_id), -70, -1.5f, UINT32_MAX - 500, true}},
      3,
      SIZE_MAX};
  mqtt_payload::NeighborsPayloadInput input = {
      10000, 3, &readNeighbors, &fixture};

  char buffer[4096];
  size_t payload_len = 0;
  ASSERT_TRUE(mqtt_payload::buildNeighborsPayload(
      buffer, sizeof(buffer), input, payload_len));
  EXPECT_EQ(std::string(buffer, payload_len),
            "{\"nodes\":[{\"id\":\"00AB10FF\",\"rssi\":-90,"
            "\"snr\":5.5,\"last_seen_s\":3},{\"id\":\"12345678\","
            "\"rssi\":-70,\"snr\":-1.5,\"last_seen_s\":10}]}");
}

TEST(MqttPayloadBuilders, NeighborsRejectReaderFailureAndShortCapacity) {
  static const uint8_t id[] = {0x01, 0x02};
  NeighborFixture fixture = {
      {{id, sizeof(id), -1, 1.0f, 0, true}, {}, {}},
      3,
      1};
  mqtt_payload::NeighborsPayloadInput input = {
      1000, 3, &readNeighbors, &fixture};
  char buffer[4096];
  size_t payload_len = 99;
  EXPECT_FALSE(mqtt_payload::buildNeighborsPayload(
      buffer, sizeof(buffer), input, payload_len));
  EXPECT_EQ(payload_len, 0u);

  fixture.fail_index = SIZE_MAX;
  size_t reference_len = 0;
  ASSERT_TRUE(mqtt_payload::buildNeighborsPayload(
      buffer, sizeof(buffer), input, reference_len));
  std::vector<char> short_buffer(reference_len, 'X');
  EXPECT_FALSE(mqtt_payload::buildNeighborsPayload(
      short_buffer.data(), short_buffer.size(), input, payload_len));
  EXPECT_EQ(payload_len, 0u);
  EXPECT_LT(std::strlen(short_buffer.data()), short_buffer.size());
}

}  // namespace
