#include <gtest/gtest.h>
#include "Mesh/Mesh.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"

using namespace planetopia::mesh;

// ReplayCache is tested via Mesh's isReplay() method.
// We construct a Mesh instance in test mode and call isReplay directly.

class ReplayCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  mesh_message makeMsg(const uint8_t mac[6], uint32_t epoch, uint16_t seq) {
    mesh_message m{};
    m.protoVersion = 1;
    m.epochNum = epoch;
    m.seqNum   = seq;
    memcpy(m.originMacAddress, mac, 6);
    return m;
  }
};

TEST_F(ReplayCacheTest, FirstMessage_NotDuplicate) {
  Mesh mesh;
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  auto m = makeMsg(mac, 1, 1);
  EXPECT_FALSE(mesh.isReplay(m));
}

TEST_F(ReplayCacheTest, SameEpochSeq_IsDuplicate) {
  Mesh mesh;
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  auto m = makeMsg(mac, 1, 10);
  EXPECT_FALSE(mesh.isReplay(m));
  EXPECT_TRUE(mesh.isReplay(m));   // Same msg again = duplicate
}

TEST_F(ReplayCacheTest, DifferentSeq_NotDuplicate) {
  Mesh mesh;
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, 10)));
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, 11)));  // Different seq
}

TEST_F(ReplayCacheTest, DifferentEpoch_SameSeq_NotDuplicate) {
  Mesh mesh;
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, 5)));
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 2, 5)));  // Same seq, different epoch
}

TEST_F(ReplayCacheTest, DifferentMAC_SameSeq_NotDuplicate) {
  Mesh mesh;
  const uint8_t mac1[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t mac2[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac1, 1, 1)));
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac2, 1, 1)));  // Different MAC
}

TEST_F(ReplayCacheTest, RingBuffer_OldestEvicted_NotDuplicate) {
  Mesh mesh;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  // Fill the ring buffer (REPLAY_CACHE_SIZE = 16 in Mesh.h)
  for (uint16_t i = 0; i < 16; ++i) {
    EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, i)));
  }
  // Seq 0 is now evicted — inserting 16 new entries pushed it out
  // Adding 16 more entries evicts seq 0
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, 16)));  // New entry, evicts seq 0
  // seq 0 should no longer be detected as duplicate (evicted)
  EXPECT_FALSE(mesh.isReplay(makeMsg(mac, 1, 0)));
}
