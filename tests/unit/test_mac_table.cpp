#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include "src/network/mac_table.h"

using lattice::mac_table::evict_oldest_by_ts;
using lattice::mac_table::find;

namespace {

// A small POD entry type standing in for the 5 real callers' entry structs —
// exercises find()/evict_oldest_by_ts() purely via stride + offset, the way
// NeighborTable/RouteTable/E2EKeyStore/ReplayCache/PeerRegistry do.
struct Entry {
  uint8_t mac[6];
  bool valid;
  uint32_t ts;
};

} // namespace

TEST(MacTableFind, FindsMatchingEntry) {
  Entry entries[3] = {
      {{1, 0, 0, 0, 0, 1}, true, 100},
      {{1, 0, 0, 0, 0, 2}, true, 200},
      {{1, 0, 0, 0, 0, 3}, true, 300},
  };
  const uint8_t target[6] = {1, 0, 0, 0, 0, 2};
  size_t idx = find(entries, 3, sizeof(Entry), offsetof(Entry, mac), target);
  EXPECT_EQ(idx, 1u);
}

TEST(MacTableFind, ReturnsSizeMaxWhenNotFound) {
  Entry entries[3] = {
      {{1, 0, 0, 0, 0, 1}, true, 100},
      {{1, 0, 0, 0, 0, 2}, true, 200},
      {{1, 0, 0, 0, 0, 3}, true, 300},
  };
  const uint8_t target[6] = {9, 9, 9, 9, 9, 9};
  size_t idx = find(entries, 3, sizeof(Entry), offsetof(Entry, mac), target);
  EXPECT_EQ(idx, SIZE_MAX);
}

TEST(MacTableFind, ReturnsSizeMaxOnEmptyTable) {
  Entry entries[1] = {{{0, 0, 0, 0, 0, 0}, false, 0}};
  const uint8_t target[6] = {1, 2, 3, 4, 5, 6};
  size_t idx = find(entries, 0, sizeof(Entry), offsetof(Entry, mac), target);
  EXPECT_EQ(idx, SIZE_MAX);
}

TEST(MacTableFind, ReturnsFirstMatchOnDuplicateMacs) {
  // Not a case any real caller produces, but find() itself must be
  // well-defined: first index wins, matching a plain linear scan.
  Entry entries[3] = {
      {{2, 0, 0, 0, 0, 9}, true, 100},
      {{2, 0, 0, 0, 0, 9}, true, 200},
      {{1, 0, 0, 0, 0, 1}, true, 300},
  };
  const uint8_t target[6] = {2, 0, 0, 0, 0, 9};
  size_t idx = find(entries, 3, sizeof(Entry), offsetof(Entry, mac), target);
  EXPECT_EQ(idx, 0u);
}

TEST(MacTableFind, MatchesEntryAtNonZeroOffset) {
  // mac_offset != 0 — confirms the byte-stride arithmetic actually honors the
  // offset parameter rather than assuming mac starts each entry.
  struct Reordered {
    uint32_t ts;
    uint8_t mac[6];
  };
  Reordered entries[2] = {
      {111, {5, 5, 5, 5, 5, 1}},
      {222, {5, 5, 5, 5, 5, 2}},
  };
  const uint8_t target[6] = {5, 5, 5, 5, 5, 2};
  size_t idx = find(entries, 2, sizeof(Reordered), offsetof(Reordered, mac), target);
  EXPECT_EQ(idx, 1u);
}

TEST(MacTableEvictOldestByTs, PicksSmallestTimestamp) {
  Entry entries[4] = {
      {{0, 0, 0, 0, 0, 0}, true, 500},
      {{0, 0, 0, 0, 0, 1}, true, 100}, // oldest
      {{0, 0, 0, 0, 0, 2}, true, 900},
      {{0, 0, 0, 0, 0, 3}, true, 300},
  };
  size_t idx = evict_oldest_by_ts(entries, 4, sizeof(Entry), offsetof(Entry, ts));
  EXPECT_EQ(idx, 1u);
}

TEST(MacTableEvictOldestByTs, TieBreaksToLowestIndex) {
  Entry entries[3] = {
      {{0, 0, 0, 0, 0, 0}, true, 100},
      {{0, 0, 0, 0, 0, 1}, true, 100}, // tie with index 0
      {{0, 0, 0, 0, 0, 2}, true, 200},
  };
  size_t idx = evict_oldest_by_ts(entries, 3, sizeof(Entry), offsetof(Entry, ts));
  EXPECT_EQ(idx, 0u);
}

TEST(MacTableEvictOldestByTs, SingleEntryReturnsZero) {
  Entry entries[1] = {{{0, 0, 0, 0, 0, 0}, true, 42}};
  size_t idx = evict_oldest_by_ts(entries, 1, sizeof(Entry), offsetof(Entry, ts));
  EXPECT_EQ(idx, 0u);
}
