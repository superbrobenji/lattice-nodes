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
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1), 1000));
}

TEST(ReplayCacheTest, DuplicateIsReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  rc.isReplay(makeMsg(mac, 1, 1), 1000);   // first — records it
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 1), 1001));  // second — replay
}

TEST(ReplayCacheTest, DifferentSeqNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  rc.isReplay(makeMsg(mac, 1, 1), 1000);
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 2), 1001));
}

TEST(ReplayCacheTest, DifferentEpochSameSeqNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 2, 5), 1001));  // Same seq, different epoch
}

TEST(ReplayCacheTest, DifferentMACNotReplay) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac1[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t mac2[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac1, 1, 1), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac2, 1, 1), 1001));  // Different MAC
}

TEST(ReplayCacheTest, SingleOrigin_MonotonicSeqBeyondOldRingSize_NeverFalsePositive) {
  // Renamed from RingBufferWrapsWithoutFalsePositive: the old CACHE_SIZE ring
  // symbol no longer exists (replaced by the per-origin table sized by
  // LATTICE_REPLAY_MAX_ORIGINS). Assertion is unchanged in strength — a
  // single origin sending strictly increasing seq numbers, well past the old
  // ring's slot count, must never be flagged as a replay.
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  for (uint16_t i = 1; i <= lattice::config::LATTICE_REPLAY_MAX_ORIGINS + 1; ++i) {
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, i), 1000 + i));
  }
}

TEST(ReplayCacheTest, HighWater_StaleSeqAfterAdvance_StillDetected) {
  // Renamed from RingBuffer_OldestEvicted_NotDuplicate. The old test
  // documented the ring's index-based eviction: after 16 intervening inserts
  // for the SAME origin, an old already-seen seq was wrongly accepted again
  // (EXPECT_FALSE) because its ring slot had been overwritten by position —
  // this is exactly the false-negative issue #46 exists to close.
  //
  // Under per-origin high-water tracking a single origin keeps exactly one
  // slot for its entire lifetime (no index-based eviction can touch it), so
  // the same sequence of sends must now be correctly rejected as a replay
  // (EXPECT_TRUE). This is a strengthening of the guarantee the original
  // test was probing, not a weakening: the old attack (replay an
  // already-superseded seq from an active origin) is now barred.
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  for (uint16_t i = 0; i < 16; ++i) {
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, i), 1000 + i));
  }
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 16), 1016));  // new high-water
  // seq 0 is now stale relative to this origin's high-water (16) — per-origin
  // tracking correctly rejects it, unlike the old ring which would have
  // accepted it once its position-based slot was overwritten.
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 0), 1017));
}

TEST(ReplayCacheTest, InitResetsState) {
  ReplayCache rc;
  rc.init(1);
  const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  rc.isReplay(makeMsg(mac, 1, 1), 1000);
  // After re-init, same message should not be a replay
  rc.init(2);
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1), 1001));
}

TEST(ReplayCacheTest, NextSeqIncrements) {
  ReplayCache rc;
  rc.init(5);
  EXPECT_EQ(rc.nextSeq(), 1);
  EXPECT_EQ(rc.nextSeq(), 2);
  EXPECT_EQ(rc.bootEpoch, 5u);
}

// --- Per-origin high-water semantics (issue #46) ---
// Reuses the makeMsg() helper defined at the top of this file.

TEST(ReplayCachePerOrigin, FirstFrame_Accepts) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1), 1000));
}

TEST(ReplayCachePerOrigin, ExactReplay_Drops) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
}

TEST(ReplayCachePerOrigin, StrictlyNewer_Accepts) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 6), 1001));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 2, 0), 1002));
}

TEST(ReplayCachePerOrigin, OutOfOrderSameOrigin_Drops) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 4), 1001));   // same epoch, lower seq
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 0, 999), 1002)); // lower epoch
}

TEST(ReplayCachePerOrigin, DifferentOrigin_DoesNotCollide) {
  ReplayCache rc; rc.init(1);
  uint8_t a[6] = {1,2,3,4,5,6};
  uint8_t b[6] = {6,5,4,3,2,1};
  EXPECT_FALSE(rc.isReplay(makeMsg(a, 1, 5), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(b, 1, 5), 1001));   // b's first frame — accept
  EXPECT_TRUE(rc.isReplay(makeMsg(a, 1, 5), 1002));    // a's replay still detected
}

TEST(ReplayCachePerOrigin, FullTable_EvictsOldest) {
  ReplayCache rc; rc.init(1);
  for (size_t i = 0; i < lattice::config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
    uint8_t mac[6] = {static_cast<uint8_t>(i+1), 0, 0, 0, 0, 0};
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 0), 1000 + i));
  }
  // Table full. Oldest is mac {1,...} with lastSeenMs=1000.
  // Insert one new origin — must evict {1,...}.
  uint8_t newMac[6] = {0xAA, 0, 0, 0, 0, 0};
  EXPECT_FALSE(rc.isReplay(makeMsg(newMac, 1, 0), 2000));
  // Now replay {1,...}'s frame — since its slot was evicted, it looks first-ever.
  uint8_t evicted[6] = {1, 0, 0, 0, 0, 0};
  EXPECT_FALSE(rc.isReplay(makeMsg(evicted, 1, 0), 2001));  // accepted (documented limitation)
}
