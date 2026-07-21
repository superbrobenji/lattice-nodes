#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/RouteTable.h"

using namespace lattice::mesh;

static const uint8_t NODE[6] = {0x02, 0, 0, 0, 0, 0xEE};
static const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
static const uint8_t R2[6] = {0x02, 0, 0, 0, 0, 0x22};

TEST(RouteTable, RecordsAndLooksUpPath) {
  RouteTable t;
  uint8_t path[12];
  memcpy(path, R1, 6);
  memcpy(path + 6, R2, 6);
  t.record(NODE, path, 2, 1000);
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(NODE, out, &outLen));
  EXPECT_EQ(outLen, 2);
  EXPECT_EQ(0, memcmp(out, R1, 6));
  EXPECT_EQ(0, memcmp(out + 6, R2, 6));
}

TEST(RouteTable, LookupMissReturnsFalse) {
  RouteTable t;
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}

TEST(RouteTable, RecordUpdatesExistingNode) {
  RouteTable t;
  uint8_t p1[6];
  memcpy(p1, R1, 6);
  t.record(NODE, p1, 1, 1000); // path via R1
  uint8_t p2[12];
  memcpy(p2, R2, 6);
  memcpy(p2 + 6, R1, 6);
  t.record(NODE, p2, 2, 2000); // newer path via R2,R1
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(NODE, out, &outLen));
  EXPECT_EQ(outLen, 2);
  EXPECT_EQ(0, memcmp(out, R2, 6));
}

TEST(RouteTable, RejectsOverlongPath) {
  RouteTable t;
  uint8_t big[66] = {};
  t.record(NODE, big, 11, 1000); // 11 > MAX_HOPS(10) → ignored
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}

TEST(RouteTable, EvictsOldestWhenFull) {
  RouteTable t;
  uint8_t p[6] = {0x02, 0, 0, 0, 0, 0x01};
  // Fill: node i observed at time (i+1)*100; node 0 is oldest.
  for (size_t i = 0; i < lattice::config::LATTICE_ROUTE_TABLE_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(0x80 + i)};
    t.record(mac, p, 1, static_cast<uint32_t>((i + 1) * 100));
  }
  uint8_t oldest[6] = {0x02, 0, 0, 0, 0, 0x80}; // observed at t=100
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(oldest, out, &outLen));
  // Insert one more (fresh) node → oldest must be evicted.
  t.record(NODE, p, 1, 999999);
  EXPECT_FALSE(t.lookup(oldest, out, &outLen)) << "oldest entry evicted";
  EXPECT_TRUE(t.lookup(NODE, out, &outLen)) << "new entry present";
}

TEST(RouteTable, ClearEmpties) {
  RouteTable t;
  uint8_t p[6] = {0x02, 0, 0, 0, 0, 0x01};
  t.record(NODE, p, 1, 1000);
  t.clear();
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}
