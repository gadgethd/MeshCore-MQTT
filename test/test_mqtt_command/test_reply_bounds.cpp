#include <cstring>

#include <gtest/gtest.h>

#include "helpers/MqttCommandReply.h"

TEST(MqttCommandReply, PrefixLeavesOnlyRemainingCapacity) {
  struct ReplyBuffer {
    char reply[160];
    unsigned char canary[4];
  } buffer{};
  std::memset(buffer.canary, 0xA5, sizeof(buffer.canary));
  std::memcpy(buffer.reply, "x| ", 3);

  char value[200];
  std::memset(value, 'V', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';

  writeMqttValueReply(buffer.reply + 3, sizeof(buffer.reply) - 3, value);

  EXPECT_EQ(std::strlen(buffer.reply), sizeof(buffer.reply) - 1);
  EXPECT_EQ(buffer.reply[sizeof(buffer.reply) - 1], '\0');
  for (unsigned char byte : buffer.canary) {
    EXPECT_EQ(byte, 0xA5);
  }
}

TEST(MqttCommandReply, ZeroCapacityDoesNotWrite) {
  char reply[] = "unchanged";
  writeMqttValueReply(reply, 0, "secret");
  EXPECT_STREQ(reply, "unchanged");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
