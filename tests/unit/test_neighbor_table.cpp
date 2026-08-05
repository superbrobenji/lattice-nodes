#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/NeighborTable.h"

using namespace lattice::mesh;

static const uint8_t A[6] = {0x02, 0, 0, 0, 0, 0xA1};
static const uint8_t B[6] = {0x02, 0, 0, 0, 0, 0xB2};
static const uint8_t C[6] = {0x02, 0, 0, 0, 0, 0xC3};

TEST(NeighborTable, SelectsCloserNeighbor) {
  NeighborTable t;
  t.observe(A, 1, 1000); // A is 1 hop from master
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 1000, out)); // own distance 2
  EXPECT_EQ(0, memcmp(out, A, 6));
}

TEST(NeighborTable, RejectsEqualOrFartherNeighbor) {
  NeighborTable t;
  t.observe(A, 2, 1000); // same distance as us
  t.observe(B, 3, 1000); // farther
  uint8_t out[6];
  EXPECT_FALSE(t.selectNextHop(2, 1000, out)); // strict < required
}

TEST(NeighborTable, PicksFreshestAmongEligible) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  t.observe(B, 1, 5000); // same distance, seen more recently
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 5000, out));
  EXPECT_EQ(0, memcmp(out, B, 6)); // freshest wins
}

TEST(NeighborTable, StaleNeighborNotEligible) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  uint8_t out[6];
  // now is 1000 + STALE + 1 → A is stale
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1;
  EXPECT_FALSE(t.selectNextHop(2, now, out));
}

TEST(NeighborTable, ExactStaleThresholdIsStale) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  uint8_t out[6];
  // age == STALE_PEER_THRESHOLD_MS exactly → the impl uses >=, so it must be INELIGIBLE.
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS;
  EXPECT_FALSE(t.selectNextHop(2, now, out)) << "age exactly == threshold is stale (>=)";
  // one ms before the threshold → still fresh.
  EXPECT_TRUE(t.selectNextHop(2, now - 1, out));
}

TEST(NeighborTable, ObserveUpdatesExistingEntry) {
  NeighborTable t;
  t.observe(A, 3, 1000);
  t.observe(A, 1, 2000); // same MAC, better distance + newer
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 2000, out));
  EXPECT_EQ(0, memcmp(out, A, 6));
}

TEST(NeighborTable, EvictsFarthestWhenFullAndNoneStale) {
  NeighborTable t;
  // Fill all slots, all fresh (t=1000), distances 2..(MAX+1) so the farthest is
  // uniquely identifiable. Slot i → mac {..,i}, distance i+2.
  uint8_t farthest[6] = {0x02, 0, 0,
                         0,    0, static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 0; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    t.observe(mac, static_cast<uint8_t>(i + 2), 1000);
  }
  ASSERT_TRUE(t.contains(farthest)); // the largest-distance entry, before eviction
  // Table full, nothing stale → inserting C must evict the farthest entry.
  t.observe(C, 1, 1000);
  EXPECT_TRUE(t.contains(C)) << "new neighbor inserted";
  EXPECT_FALSE(t.contains(farthest)) << "farthest-from-master entry evicted";
}

TEST(NeighborTable, EvictsStaleBeforeFarthest) {
  NeighborTable t;
  // slot 0: a CLOSE neighbor (distance 2) observed long ago → will be stale.
  // slots 1..MAX-1: FARTHER neighbors (distance 6+) observed recently → fresh.
  // Inserting C must evict the stale close one, NOT the fresh farthest one.
  uint8_t stale[6] = {0x02, 0, 0, 0, 0, 0x00};
  t.observe(stale, 2, 1000); // old
  uint8_t freshFarthest[6] = {
      0x02, 0, 0, 0, 0, static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 1; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    t.observe(mac, static_cast<uint8_t>(i + 5), 6000); // farther, observed recently
  }
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1; // 9001
  // At now: stale (age 8001) is stale; the t=6000 entries (age 3001) are fresh.
  t.observe(C, 1, now);
  EXPECT_TRUE(t.contains(C));
  EXPECT_FALSE(t.contains(stale)) << "stale entry evicted first";
  EXPECT_TRUE(t.contains(freshFarthest)) << "fresh farthest entry survives — stale beats farthest";
}

TEST(NeighborTable, ClearEmptiesTable) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  t.clear();
  uint8_t out[6];
  EXPECT_FALSE(t.selectNextHop(2, 1000, out));
}

// --- minFreshDistance (issue #45) ---

class NeighborTableTest : public ::testing::Test {};

TEST_F(NeighborTableTest, MinFreshDistance_Empty_Returns0xFF) {
  NeighborTable nt;
  EXPECT_EQ(nt.minFreshDistance(1000), 0xFF);
}

TEST_F(NeighborTableTest, MinFreshDistance_SingleFresh_ReturnsIt) {
  NeighborTable nt;
  uint8_t mac[6] = {1,2,3,4,5,6};
  nt.observe(mac, 3, 1000);
  EXPECT_EQ(nt.minFreshDistance(1000), 3);
}

TEST_F(NeighborTableTest, MinFreshDistance_MultipleFresh_ReturnsMin) {
  NeighborTable nt;
  uint8_t m1[6] = {1,0,0,0,0,1};
  uint8_t m2[6] = {1,0,0,0,0,2};
  uint8_t m3[6] = {1,0,0,0,0,3};
  nt.observe(m1, 5, 1000);
  nt.observe(m2, 2, 1000);
  nt.observe(m3, 4, 1000);
  EXPECT_EQ(nt.minFreshDistance(1000), 2);
}

TEST_F(NeighborTableTest, MinFreshDistance_AllStale_Returns0xFF) {
  NeighborTable nt;
  uint8_t mac[6] = {1,2,3,4,5,6};
  nt.observe(mac, 3, 1000);
  uint32_t future = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1;
  EXPECT_EQ(nt.minFreshDistance(future), 0xFF);
}

TEST_F(NeighborTableTest, MinFreshDistance_MixedFreshStale_IgnoresStale) {
  NeighborTable nt;
  uint8_t stale[6] = {1,0,0,0,0,1};
  uint8_t fresh[6] = {1,0,0,0,0,2};
  nt.observe(stale, 1, 1000);
  nt.observe(fresh, 4, 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1);
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 2;
  EXPECT_EQ(nt.minFreshDistance(now), 4);   // stale's 1 ignored
}

// --- observeAndMinDistance (post-Phase-G audit item X: insert+fold in one pass) ---
// Each case cross-checks against the observe()+minFreshDistance() combo it replaces
// in Mesh::processMasterBeacon, on an independently-built table so the two never
// interfere with each other.

TEST_F(NeighborTableTest, ObserveAndMinDistance_Empty_InsertsAndReturnsOwnDistance) {
  NeighborTable nt;
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  EXPECT_EQ(nt.observeAndMinDistance(mac, 3, 1000), 3);
  EXPECT_TRUE(nt.contains(mac));
}

TEST_F(NeighborTableTest, ObserveAndMinDistance_MatchesSeparateCalls_MultipleFresh) {
  // Cross-check against the observe()+minFreshDistance() combo this replaces
  // in Mesh::processMasterBeacon: same inputs, same final min, on independent
  // tables so neither run affects the other.
  NeighborTable direct;
  NeighborTable folded;
  uint8_t m1[6] = {1, 0, 0, 0, 0, 1};
  uint8_t m2[6] = {1, 0, 0, 0, 0, 2};
  uint8_t m3[6] = {1, 0, 0, 0, 0, 3};

  direct.observe(m1, 5, 1000);
  direct.observe(m2, 2, 1000);
  direct.observe(m3, 4, 1000);
  uint8_t directResult = direct.minFreshDistance(1000);

  folded.observeAndMinDistance(m1, 5, 1000);
  folded.observeAndMinDistance(m2, 2, 1000);
  uint8_t foldedResult = folded.observeAndMinDistance(m3, 4, 1000);

  EXPECT_EQ(directResult, 2);
  EXPECT_EQ(foldedResult, 2);
  EXPECT_EQ(foldedResult, directResult);
}

TEST_F(NeighborTableTest, ObserveAndMinDistance_UpdatingExistingEntry_UsesNewDistanceNotStale) {
  // Re-observing the same MAC with a worse distance must not let the OLD
  // (better) distance leak into the returned min via a stale bookkeeping bug.
  NeighborTable nt;
  uint8_t mac[6] = {2, 0, 0, 0, 0, 9};
  nt.observeAndMinDistance(mac, 1, 1000); // first: distance 1
  uint8_t result = nt.observeAndMinDistance(mac, 5, 2000); // update: distance 5, only entry
  EXPECT_EQ(result, 5);
}

TEST_F(NeighborTableTest, ObserveAndMinDistance_IgnoresStaleOthers) {
  NeighborTable nt;
  uint8_t stale[6] = {1, 0, 0, 0, 0, 1};
  uint8_t mac[6] = {1, 0, 0, 0, 0, 2};
  nt.observeAndMinDistance(stale, 1, 1000);
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1;
  uint8_t result = nt.observeAndMinDistance(mac, 4, now); // stale[distance 1] must be ignored
  EXPECT_EQ(result, 4);
}

TEST_F(NeighborTableTest, ObserveAndMinDistance_EvictsFarthestWhenFullAndNoneStale) {
  NeighborTable nt;
  uint8_t farthest[6] = {0x02, 0, 0,
                         0,    0, static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 0; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    nt.observeAndMinDistance(mac, static_cast<uint8_t>(i + 2), 1000);
  }
  ASSERT_TRUE(nt.contains(farthest));
  uint8_t result = nt.observeAndMinDistance(C, 1, 1000);
  EXPECT_TRUE(nt.contains(C)) << "new neighbor inserted";
  EXPECT_FALSE(nt.contains(farthest)) << "farthest-from-master entry evicted";
  EXPECT_EQ(result, 1) << "new (closest) neighbor's own distance is the new min";
}

TEST_F(NeighborTableTest, ObserveAndMinDistance_EvictsStaleBeforeFarthest) {
  NeighborTable nt;
  uint8_t stale[6] = {0x02, 0, 0, 0, 0, 0x00};
  nt.observeAndMinDistance(stale, 2, 1000); // old, will be stale
  uint8_t freshFarthest[6] = {
      0x02, 0, 0, 0, 0, static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 1; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    nt.observeAndMinDistance(mac, static_cast<uint8_t>(i + 5), 6000); // farther, recent
  }
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1; // 9001
  uint8_t result = nt.observeAndMinDistance(C, 1, now);
  EXPECT_TRUE(nt.contains(C));
  EXPECT_FALSE(nt.contains(stale)) << "stale entry evicted first, not the fresh farthest one";
  EXPECT_TRUE(nt.contains(freshFarthest));
  EXPECT_EQ(result, 1);
}
