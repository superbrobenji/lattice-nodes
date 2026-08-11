#include <cstring>
#include <gtest/gtest.h>
#include "mesh/UplinkRouter.h"

using namespace lattice::mesh;

TEST(UplinkRouterTest, ReturnsNullWhenNoMasterKnown) {
  UplinkRouter router;
  MasterInfo currentMaster{};
  currentMaster.distance = 0xFF;
  PeerRegistry peers;
  NeighborTable neighbors;
  uint8_t deviceMac[6] = {1, 1, 1, 1, 1, 1};
  EXPECT_EQ(router.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac, 1000), nullptr);
}

TEST(UplinkRouterTest, ReturnsDirectPeerWhenInRangeAtDistanceOne) {
  UplinkRouter router;
  MasterInfo currentMaster{};
  memset(currentMaster.mac, 0xAA, 6);
  currentMaster.distance = 1;
  PeerRegistry peers;
  PeerInfo master{};
  memcpy(master.mac, currentMaster.mac, 6);
  // PeerRegistry::isPeerInRange() compares against the mocked global clock
  // (esp_timer_get_time(), unadvanced here — starts at 0 for this test
  // binary), not the `nowMs` parameter below (that only feeds
  // NeighborTable::selectNextHop for the multi-hop branch) — lastSeenMs must
  // be <= the mocked "now" or the unsigned subtraction in isPeerInRange()
  // underflows and the peer reads as stale.
  master.lastSeenMs = 0;
  peers.append(master);
  NeighborTable neighbors;
  uint8_t deviceMac[6] = {1, 1, 1, 1, 1, 1};
  PeerInfo* hop = router.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac, 1000);
  ASSERT_NE(hop, nullptr);
  EXPECT_EQ(memcmp(hop->mac, currentMaster.mac, 6), 0);
}
