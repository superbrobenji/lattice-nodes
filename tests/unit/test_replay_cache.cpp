#include <gtest/gtest.h>
#include "mesh/ReplayCache.h"

using namespace lattice::mesh;

static mesh_message makeMsg(const uint8_t mac[6], uint32_t epoch, uint16_t seq) {
  mesh_message m{};
  m.proto_version = 1;
  m.epoch_num = epoch;
  m.seq_num   = seq;
  memcpy(m.origin_mac_address, mac, 6);
  return m;
}

TEST(ReplayCacheTest, FreshMessageNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1)));
}

TEST(ReplayCacheTest, DuplicateIsReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  rc.isReplay(makeMsg(mac, 1, 1));   // first — records it
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 1)));  // second — replay
}

TEST(ReplayCacheTest, DifferentSeqNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  rc.isReplay(makeMsg(mac, 1, 1));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 2)));
}

TEST(ReplayCacheTest, DifferentEpochSameSeqNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5)));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 2, 5)));  // Same seq, different epoch
}

TEST(ReplayCacheTest, DifferentMACNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac1[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t mac2[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac1, 1, 1)));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac2, 1, 1)));  // Different MAC
}

TEST(ReplayCacheTest, RingBufferWrapsWithoutFalsePositive) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  // Fill cache entirely plus one more (wraps ring buffer)
  for (uint16_t i = 1; i <= ReplayCache::CACHE_SIZE + 1; ++i) {
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, i)));
  }
}

TEST(ReplayCacheTest, RingBuffer_OldestEvicted_NotDuplicate) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  // Fill the ring buffer (CACHE_SIZE = 16)
  for (uint16_t i = 0; i < 16; ++i) {
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, i)));
  }
  // Seq 0 is now evicted — inserting 16 new entries pushed it out
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 16)));  // New entry, evicts seq 0
  // seq 0 should no longer be detected as duplicate (evicted)
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 0)));
}

TEST(ReplayCacheTest, InitResetsState) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  rc.isReplay(makeMsg(mac, 1, 1));
  // After re-init, same message should not be a replay
  rc.init(2);
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1)));
}

TEST(ReplayCacheTest, NextSeqIncrements) {
  ReplayCache rc;
  rc.init(5);
  EXPECT_EQ(rc.nextSeq(), 1);
  EXPECT_EQ(rc.nextSeq(), 2);
  EXPECT_EQ(rc.bootEpoch, 5u);
}
