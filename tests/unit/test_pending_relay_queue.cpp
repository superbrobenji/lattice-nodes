#include <gtest/gtest.h>
#include "mesh/PendingRelayQueue.h"

using namespace lattice::mesh;

namespace {
std::vector<std::pair<uint8_t, uint8_t>> g_drained; // (mac[0], pubKey[0]) pairs for identity checks

void recordDrain(const uint8_t* mac, const uint8_t* pubKey) {
  g_drained.emplace_back(mac[0], pubKey[0]);
}
} // namespace

TEST(PendingRelayQueueTest, PushThenDrainDeliversInOrder) {
  g_drained.clear();
  PendingRelayQueue q;
  uint8_t mac1[6] = {1, 0, 0, 0, 0, 0}, pk1[32] = {11};
  uint8_t mac2[6] = {2, 0, 0, 0, 0, 0}, pk2[32] = {22};
  q.push(mac1, pk1);
  q.push(mac2, pk2);
  q.drainTo(recordDrain);
  ASSERT_EQ(g_drained.size(), 2u);
  EXPECT_EQ(g_drained[0], (std::pair<uint8_t, uint8_t>{1, 11}));
  EXPECT_EQ(g_drained[1], (std::pair<uint8_t, uint8_t>{2, 22}));
}

TEST(PendingRelayQueueTest, DrainOnEmptyQueueCallsNothing) {
  g_drained.clear();
  PendingRelayQueue q;
  q.drainTo(recordDrain);
  EXPECT_TRUE(g_drained.empty());
}

TEST(PendingRelayQueueTest, DrainEmptiesTheQueue) {
  g_drained.clear();
  PendingRelayQueue q;
  uint8_t mac[6] = {5, 0, 0, 0, 0, 0}, pk[32] = {55};
  q.push(mac, pk);
  q.drainTo(recordDrain);
  g_drained.clear();
  q.drainTo(recordDrain); // second drain — nothing left
  EXPECT_TRUE(g_drained.empty());
}
