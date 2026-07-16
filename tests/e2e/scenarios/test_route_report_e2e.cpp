// Task 10: route report e2e scenarios.
//
// A non-master node periodically emits a MESH_TYPE_ROUTE_REPORT
// (ROUTE_REPORT_INTERVAL_MS) carrying OP_ROUTE_REPORT and a hop-chain that each
// relay appends its own MAC to. Payload layout (verified against
// Mesh::sendRouteReport / processRouteReport, matches opcodes.h):
//   data[0]      = OP_ROUTE_REPORT (0xB3)
//   data[1]      = path_len (number of relay hops appended so far; the
//                  originating node emits 0 and does NOT append itself)
//   data[2 + i*6]= MAC of the i-th relay hop
//
// The originating node's own MAC travels in origin_mac_address, not in the
// path; the path accumulates only the intermediate relays between it and the
// master.
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "project_config.h"

// A directly-linked (distance-1) enrolled node's route report reaches the hub
// with the right opcode and origin, and an empty hop chain (no relays between
// it and the master).
TEST_F(MeshSimTest, DirectNodeRouteReportReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool sawRoute = false;
  for (const auto& m : hub->ofType(MESH_TYPE_ROUTE_REPORT)) {
    if (memcmp(m.origin_mac_address, sensor->mac(), 6) != 0)
      continue;
    EXPECT_EQ(m.data[0], OP_ROUTE_REPORT);
    EXPECT_EQ(m.data[1], 0) << "direct node -> master: no relay hops in the path chain";
    sawRoute = true;
  }
  EXPECT_TRUE(sawRoute) << "a direct node's route report must reach the hub";
}

// Multi-hop hop-chain: leaf (distance 2) emits a route report that the relay
// appends its MAC to before forwarding to the master, so the hub sees
// path_len >= 1 with the relay's MAC first.
//
// DISABLED: this requires multi-hop DATA uplink (leaf -> relay -> master for a
// node-originated frame). The current firmware cannot route data at hop
// distance >= 2 — sendRouteReport() returns false because findNextHopToMaster()
// is null when the next hop (the relay) is not a registered, in-range peer.
// Same root cause as the multi-hop data-uplink gap documented in
// docs/design-gaps/multihop-data-uplink.md. Assertions below are the intended
// behavior; drop the DISABLED_ prefix once that gap is closed.
TEST_F(MeshSimTest, DISABLED_RouteReportCarriesHopChain) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);
  enroll(relay);
  enroll(leaf);

  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool sawLeafRoute = false;
  for (const auto& m : hub->ofType(MESH_TYPE_ROUTE_REPORT)) {
    if (memcmp(m.origin_mac_address, leaf->mac(), 6) != 0)
      continue;
    ASSERT_EQ(m.data[0], OP_ROUTE_REPORT);
    uint8_t pathLen = m.data[1];
    ASSERT_GE(pathLen, 1);
    // First hop MAC in the chain must be the relay node.
    EXPECT_EQ(memcmp(&m.data[2], relay->mac(), 6), 0);
    sawLeafRoute = true;
  }
  EXPECT_TRUE(sawLeafRoute) << "leaf's route report must reach the hub via the relay";
}
