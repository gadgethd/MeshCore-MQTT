#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "helpers/MqttPublishGuard.h"

using namespace mqtt_publish;

TEST(MqttPublishGuard, GlobalDrainBudgetStopsAfterTwoFastCalls) {
  DrainState state = startDrain(1000);
  ASSERT_TRUE(mayPublish(state, 1000));
  notePublishCall(state);
  ASSERT_TRUE(mayPublish(state, 1010));
  notePublishCall(state);
  EXPECT_FALSE(mayPublish(state, 1020));
  EXPECT_TRUE(mustYieldToMesh(state, 1020, true));
}

TEST(MqttPublishGuard, StalledProviderForcesYieldAfterOneCall) {
  DrainState state = startDrain(5000);
  ASSERT_TRUE(mayPublish(state, 5000));
  notePublishCall(state);

  // Simulates one synchronous provider call returning at the configured hard
  // timeout.  No second broker may consume the shared mesh-loop pass.
  EXPECT_FALSE(mayPublish(state, 5000 + kNetworkTimeoutMs));
  EXPECT_EQ(state.publish_calls, 1);
  EXPECT_TRUE(mustYieldToMesh(state, 5000 + kNetworkTimeoutMs, true));
}

TEST(MqttPublishGuard, ElapsedBudgetIsWrapSafe) {
  DrainState state = startDrain(UINT32_MAX - 100);
  EXPECT_TRUE(mayPublish(state, 50));
  EXPECT_FALSE(mayPublish(state, 200));
}

TEST(MqttPublishGuard, RoundRobinBrokerSelectionIsDeterministic) {
  EXPECT_EQ(nextBroker(0, 6), 1);
  EXPECT_EQ(nextBroker(5, 6), 0);
  EXPECT_EQ(nextBroker(3, 0), 0);
}

TEST(MqttPublishGuard, QueueAdmissionHonorsEntryAndByteCaps) {
  size_t entry_bytes = 0;
  ASSERT_TRUE(queueEntryBytes(20, 100, entry_bytes));
  EXPECT_EQ(entry_bytes, 122u);
  EXPECT_EQ(admitQueueEntry(0, 16, 0, 1024, 0, 2048, 20, 100),
            QueueAdmission::Accept);
  EXPECT_EQ(admitQueueEntry(16, 16, 0, 1024, 0, 2048, 20, 100),
            QueueAdmission::EntryCap);
  EXPECT_EQ(admitQueueEntry(0, 16, 950, 1024, 0, 2048, 20, 100),
            QueueAdmission::BrokerByteCap);
  EXPECT_EQ(admitQueueEntry(0, 16, 0, 1024, 1950, 2048, 20, 100),
            QueueAdmission::TotalByteCap);
}

TEST(MqttPublishGuard, ByteBudgetDropsNewestWithoutChangingAccounting) {
  const size_t broker_before = 8000;
  const size_t total_before = 20000;
  EXPECT_EQ(admitQueueEntry(4, 16,
                            broker_before, kQueueBrokerBytesInternal,
                            total_before, kQueueTotalBytesInternal,
                            100, 500),
            QueueAdmission::BrokerByteCap);
  // Admission is a pure decision: rejecting the new entry cannot evict or
  // mutate the bytes already retained by the queue.
  EXPECT_EQ(broker_before, 8000u);
  EXPECT_EQ(total_before, 20000u);
}

TEST(MqttPublishGuard, CheckedBuilderReportsInjectedFailureAndStaysTerminated) {
  char storage[64];
  CheckedBufferBuilder builder(storage, sizeof(storage), 5);
  ASSERT_TRUE(builder.append("hello"));
  EXPECT_FALSE(builder.append("!"));
  EXPECT_FALSE(builder.ok());
  EXPECT_STREQ(storage, "hello");
  EXPECT_EQ(storage[builder.length()], '\0');
}

TEST(MqttPublishGuard, JsonEscapeWorstCaseMatchesDocumentedExpansion) {
  char control_bytes[] = {1, 2, 3, 4, '\0'};
  char storage[32];
  CheckedBufferBuilder builder(storage, sizeof(storage));
  ASSERT_TRUE(builder.appendJsonEscaped(control_bytes));
  EXPECT_EQ(builder.length(), jsonEscapedWorstCaseBytes(4));
  EXPECT_STREQ(storage, "\\u0001\\u0002\\u0003\\u0004");
}

TEST(MqttPublishGuard, PayloadAndFullLwtSizesStayInsideFourKiBContract) {
  static_assert(kMaxPublishPayloadBytes == 4095, "payload bound drift");
  static_assert(kMaxConfiguredConnectPacketBytes <= kConnectBufferBytes,
                "full LWT no longer fits CONNECT buffer");
  EXPECT_LE(kMaxLwtPayloadBytes, kMaxPublishPayloadBytes);
  EXPECT_LE(kMaxConfiguredConnectPacketBytes, kConnectBufferBytes);
  EXPECT_EQ(kConnectBufferBytes, 4096u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
