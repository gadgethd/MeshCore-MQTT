#include <cstring>

#include <gtest/gtest.h>

#include "helpers/CLIInput.h"

TEST(SerialInputBuffer, HandlesCrLfAndAcceptsNextCommand) {
  char storage[16];
  SerialInputBuffer input(storage, sizeof(storage));

  EXPECT_EQ(input.append('a'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('\n'), SerialInputBuffer::AppendResult::Ignored);
  EXPECT_EQ(input.append('b'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('\r'), SerialInputBuffer::AppendResult::LineReady);
  EXPECT_TRUE(input.lineReady());
  EXPECT_EQ(input.size(), 2u);
  EXPECT_STREQ(input.data(), "ab");

  input.finishLine();
  EXPECT_FALSE(input.lineReady());
  EXPECT_EQ(input.append('\n'), SerialInputBuffer::AppendResult::Ignored);
  EXPECT_EQ(input.append('n'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('e'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('x'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('t'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('\r'), SerialInputBuffer::AppendResult::LineReady);
  EXPECT_STREQ(input.data(), "next");
}

TEST(SerialInputBuffer, FullBoundaryStaysTerminatedAndRecovers) {
  struct GuardedStorage {
    char data[160];
    unsigned char canary[8];
  } storage{};
  std::memset(storage.canary, 0xA5, sizeof(storage.canary));
  SerialInputBuffer input(storage.data, sizeof(storage.data));

  for (size_t i = 0; i < sizeof(storage.data) - 1; ++i) {
    const char c = static_cast<char>('a' + (i % 26));
    const auto result = input.append(c);
    if (i < sizeof(storage.data) - 2) {
      EXPECT_EQ(result, SerialInputBuffer::AppendResult::Appended);
    } else {
      EXPECT_EQ(result, SerialInputBuffer::AppendResult::LineReady);
    }
  }

  EXPECT_TRUE(input.lineReady());
  EXPECT_EQ(input.size(), sizeof(storage.data) - 1);
  EXPECT_EQ(storage.data[sizeof(storage.data) - 1], '\0');
  for (unsigned char byte : storage.canary) EXPECT_EQ(byte, 0xA5);

  // The terminator belonging to the truncated line is consumed without
  // creating an empty command, and the following command is accepted.
  input.finishLine();
  EXPECT_EQ(input.append('\r'), SerialInputBuffer::AppendResult::Ignored);
  EXPECT_EQ(input.append('o'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('k'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('\r'), SerialInputBuffer::AppendResult::LineReady);
  EXPECT_STREQ(input.data(), "ok");
  for (unsigned char byte : storage.canary) EXPECT_EQ(byte, 0xA5);
}

TEST(SerialInputBuffer, ExactSmallCapacityPreservesNul) {
  struct GuardedStorage {
    char data[4];
    unsigned char canary[4];
  } storage{};
  std::memset(storage.canary, 0x5A, sizeof(storage.canary));
  SerialInputBuffer input(storage.data, sizeof(storage.data));

  EXPECT_EQ(input.append('x'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('y'), SerialInputBuffer::AppendResult::Appended);
  EXPECT_EQ(input.append('z'), SerialInputBuffer::AppendResult::LineReady);
  EXPECT_EQ(input.size(), 3u);
  EXPECT_EQ(storage.data[3], '\0');
  for (unsigned char byte : storage.canary) EXPECT_EQ(byte, 0x5A);
}

TEST(SecretCommandInput, PreservesMaskingClassification) {
  const char *secret[] = {
      "password ",
      "guest.password ",
      "set guest.password ",
      "set bridge.secret ",
      "set mqtt.wifi.pass ",
      "set mqtt.password ",
      "set mqtt.some.password ",
  };
  for (const char *line : secret) {
    EXPECT_TRUE(cli_input::isSecretCommandInput(line)) << line;
  }

  const char *non_secret[] = {
      nullptr,
      "get name",
      "password",
      "set mqtt.wifi.pass",
      "set mqtt.some.value secret",
  };
  for (const char *line : non_secret) {
    EXPECT_FALSE(cli_input::isSecretCommandInput(line)) << (line ? line : "<null>");
  }
}

TEST(AirtimeFactorParser, AcceptsFiniteValuesInRange) {
  const char *valid[] = {"0", "9", "2.5", "+1.25", "1e0", " 3.75 \t"};
  for (const char *text : valid) {
    float value = -1.0f;
    EXPECT_TRUE(cli_input::parseAirtimeFactor(text, value)) << text;
    EXPECT_GE(value, 0.0f);
    EXPECT_LE(value, 9.0f);
  }
}

TEST(AirtimeFactorParser, RejectsMalformedNonFiniteAndOutOfRangeValues) {
  const char *invalid[] = {
      "", "   ", "nan", "NaN", "inf", "-inf", "-0.1", "9.01",
      "1.5junk", "1.5.0", "1e999", "1e-999", nullptr};
  for (const char *text : invalid) {
    float value = 42.0f;
    EXPECT_FALSE(cli_input::parseAirtimeFactor(text, value)) << (text ? text : "<null>");
    EXPECT_FLOAT_EQ(value, 42.0f);
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
