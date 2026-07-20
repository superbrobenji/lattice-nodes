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
