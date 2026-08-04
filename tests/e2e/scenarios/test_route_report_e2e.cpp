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
#include "src/adapter/Adapter.h"
#include "mesh/Mesh.h"

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
// appends its MAC to (in the plaintext route_path header, spec §4) before
// forwarding to the master, so the master learns a path with the relay's MAC
// first.
//
// Header-field design (spec §4): the route report's opcode payload
// (data[0]/data[1]) is E2E-sealed origin->master, so a relay can no longer
// read/append to it (Mesh::processRouteReport's relay branch just forwards the
// sealed frame unmodified) — that's why data[1] is a reserved constant 0 (see
// DirectNodeRouteReportReachesHub above) regardless of hop count. Path
// accumulation instead happens in the plaintext mesh_message header fields
// route_len/route_path, which each relay appends to before re-transmitting.
//
// Those header fields are mesh-internal (ESP-NOW) only, not part of the
// serial wire protocol to the hub (SerialFraming/mesh.pb.h carry no
// route_len/route_path field), so hub->ofType() frames can't be used to
// inspect them — read the path the master actually recorded instead
// (Mesh::processRouteReport -> RouteTable, Task 5's "master records node
// routes from route reports").
TEST_F(MeshSimTest, RouteReportCarriesHopChain) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);
  enroll(relay);
  enroll(leaf);

  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool found = false;
  uint8_t path[lattice::config::MAX_HOPS * 6] = {};
  uint8_t pathLen = 0;
  master->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    lattice::mesh::RouteTable* routes = m.testRoutes();
    found = routes && routes->lookup(leaf->mac(), path, &pathLen);
    return 0;
  });
  ASSERT_TRUE(found) << "master must record a route to the leaf from its route reports";
  ASSERT_GE(pathLen, 1);
  EXPECT_EQ(memcmp(&path[0], relay->mac(), 6), 0)
      << "first relay in the leaf's path is the middle node";

  bool sawLeafRoute = false;
  for (const auto& m : hub->ofType(MESH_TYPE_ROUTE_REPORT)) {
    if (memcmp(m.origin_mac_address, leaf->mac(), 6) != 0)
      continue;
    sawLeafRoute = true;
  }
  EXPECT_TRUE(sawLeafRoute) << "leaf's route report reaches the hub carrying its relay path";
}

// Phase C (spec §4 / issue #44): chain-MAC end-to-end over a 2-relay chain —
// leaf -> relay2 -> relay1 -> master. Each relay folds its own hop into
// msg.auth_path (keyed off its own pairwise k_up with the master, learned at
// enrollment — no new provisioning); the master reconstructs the same chain
// from each hop's k_up and only records the path on a successful verify.
TEST_F(MeshSimTest, RouteReportEndToEnd_ValidChain) {
  addMaster();
  auto* relay1 = addSensor(MAC_NODE_A);
  static const uint8_t MAC_NODE_C[6] = {0x02, 0, 0, 0, 0, 0x0C};
  auto* relay2 = addSensor(MAC_NODE_C);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay1);
  world.bus.link(relay1, relay2);
  world.bus.link(relay2, leaf);
  enroll(relay1);
  enroll(relay2);
  enroll(leaf);

  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool found = false;
  uint8_t path[lattice::config::MAX_HOPS * 6] = {};
  uint8_t pathLen = 0;
  master->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    lattice::mesh::RouteTable* routes = m.testRoutes();
    found = routes && routes->lookup(leaf->mac(), path, &pathLen);
    return 0;
  });
  ASSERT_TRUE(found) << "master must record a chain-MAC-verified 2-hop route to the leaf";
  ASSERT_EQ(pathLen, 2);
  // route_path is accumulated hop-by-hop starting from the leaf's side: the
  // relay adjacent to the leaf (relay2) appends first (index 0), then the
  // relay adjacent to the master (relay1) appends second (index 1) — mirrors
  // RouteReportCarriesHopChain's single-relay case, extended to two hops.
  EXPECT_EQ(memcmp(&path[0], relay2->mac(), 6), 0)
      << "first path entry is the relay adjacent to the leaf";
  EXPECT_EQ(memcmp(&path[6], relay1->mac(), 6), 0)
      << "second path entry is the relay adjacent to the master";
}

// Phase C (spec §4 / issue #44): a relay corrupting the accumulated chain MAC
// before forwarding must cause the master to drop the report — never record
// the (unauthenticated) path. Route reports are periodic (no single-step
// send trigger like PirAdapter::simulateMotion()), so this polls tick-by-tick
// until the fully-chained (route_len == 2) frame from the last relay appears
// on the wire, then mutates it in that one-step window before delivery —
// same "on the bus" tamper technique as test_e2e_aead.cpp's
// TamperedFrameIsDropped, standing in for a compromised relay's own send
// (the sim harness has no first-class "evil node" concept).
TEST_F(MeshSimTest, RouteReportEndToEnd_TamperedByEvilRelay) {
  addMaster();
  auto* relay1 = addSensor(MAC_NODE_A);
  static const uint8_t MAC_NODE_C[6] = {0x02, 0, 0, 0, 0, 0x0C};
  auto* relay2 = addSensor(MAC_NODE_C);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay1);
  world.bus.link(relay1, relay2);
  world.bus.link(relay2, leaf);
  enroll(relay1);
  enroll(relay2);
  enroll(leaf);

  mesh_message* onWire = nullptr;
  for (uint32_t t = 0;
       t < lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000 && onWire == nullptr; ++t) {
    world.step(1);
    if (hub)
      hub->poll();
    mesh_message* candidate = world.bus.lastPendingOfType(MESH_TYPE_ROUTE_REPORT);
    if (candidate && candidate->route_len == 2 &&
        memcmp(candidate->origin_mac_address, leaf->mac(), 6) == 0) {
      onWire = candidate;
    }
  }
  ASSERT_NE(onWire, nullptr)
      << "the fully-chained (2-hop) route report from the last relay must appear on the wire";
  onWire->auth_path[0] ^= 0xFF; // evil relay corrupts the chain MAC before it reaches master

  runPolled(2000); // let the tampered frame be delivered to and processed by the master

  bool found = false;
  uint8_t path[lattice::config::MAX_HOPS * 6] = {};
  uint8_t pathLen = 0;
  master->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    lattice::mesh::RouteTable* routes = m.testRoutes();
    found = routes && routes->lookup(leaf->mac(), path, &pathLen);
    return 0;
  });
  EXPECT_FALSE(found)
      << "a route report with a chain MAC corrupted by a relay must be dropped, never recorded";
}
