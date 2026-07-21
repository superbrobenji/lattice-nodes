// Task 9 / 9b: multi-hop relay e2e scenarios.
//
// hop_count convention (verified against firmware, see Task 8b report and
// test_pir_dataflow_e2e.cpp): Mesh::buildMessage stamps hop_count = 0 at
// origination; it is incremented ONLY when an intermediate node relays the
// frame onward. So a chain leaf -> relay -> master (one relay hop) arrives at
// the hub with hop_count == 1, not 2 — the original brief's "== 2" reflected a
// hops-travelled model the firmware does not use.
//
// Task 9b fixes exercised here:
//   Bug #5 — non-master nodes now relay ENROLLMENT requests toward the master
//            (Mesh::relayEnrollmentUplink) and re-broadcast JOIN_ACKs so a node
//            out of direct RF range of the master can complete enrollment.
//   Bug #6 — the master queues concurrent enrollment relays (Enrollment FIFO)
//            instead of latching a single slot, so two nodes enrolling at once
//            no longer starve each other.
#include "harness/MeshSimTest.h"
#include "src/adapter/Adapter.h"
#include "mesh/Mesh.h"

namespace {
// Count enrollment-relay frames the hub received for a given origin MAC.
size_t hubEnrollmentFrames(sim::FakeHub* hub, const uint8_t* mac) {
  size_t n = 0;
  for (const auto& m : hub->received)
    if (m.message_type == MESH_TYPE_ENROLLMENT && memcmp(m.origin_mac_address, mac, 6) == 0)
      ++n;
  return n;
}
} // namespace

// Task 9c R1: in a relay-path topology the master hears one logical enrollment
// request both directly AND re-relayed by a neighbour, but must forward it to the
// hub exactly once (ReplayCache dedups the relayed copy by origin/epoch/seq).
// Both nodes are enrolled first so neither auto-retries — this isolates a SINGLE
// request round deterministically (the existing triangle test cannot, since it
// never counts hub enrollment frames).
TEST_F(MeshSimTest, EnrollmentRequestNotDuplicatedToHubViaRelayPath) {
  addMaster();
  auto* a = addSensor(MAC_NODE_A);
  auto* b = addSensor(MAC_NODE_B);
  world.bus.link(master, a);
  world.bus.link(master, b);
  world.bus.link(a, b);
  enroll(a);
  enroll(b); // both enrolled -> no auto-retry broadcasts interfere below

  size_t before = hubEnrollmentFrames(hub.get(), b->mac());
  // Fire exactly ONE fresh enrollment request from b. The master hears it
  // directly and also re-relayed by the (enrolled) neighbour a.
  b->with([](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    m.sendEnrollmentRequest();
    return 0;
  });
  runPolled(1500);
  size_t after = hubEnrollmentFrames(hub.get(), b->mac());
  EXPECT_EQ(after - before, 1u)
      << "one logical enrollment request must reach the hub exactly once (relayed copy deduped)";
}

// Full sensor-out-of-master-range chain: leaf enrolls THROUGH the relay (Bug #5
// uplink relay + JOIN_ACK re-broadcast) and its PIR event travels
// leaf -> relay -> master to the hub.
//
// Multi-hop data uplink (gap #7, closed in Phase 2). A leaf out of direct RF
// range of the master enrolls through the relay (Phase 1 enrollment relay) and
// now uplinks sealed adapter data through it: the leaf learns the relay as a
// distance-1 neighbor from relayed beacons (NeighborTable, spec §3),
// findNextHopToMaster() selects it and auto-registers it as an unencrypted
// ESP-NOW peer, and the relay forwards the sealed frame it cannot read.
TEST_F(MeshSimTest, SensorOutOfMasterRangeRelaysThroughMiddleNode) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);

  enroll(relay);
  enroll(leaf);

  size_t before = hub->adapterDataFromOrigin(leaf->mac()).size();
  leaf->simulatePirMotion();
  runPolled(3000);

  auto frames = hub->adapterDataFromOrigin(leaf->mac());
  ASSERT_GT(frames.size(), before);
  const mesh_message& f = frames.back();
  EXPECT_EQ(f.hop_count, 1) << "leaf -> relay -> master: one relay hop";
  EXPECT_EQ(memcmp(f.origin_mac_address, leaf->mac(), 6), 0);
  EXPECT_EQ(memcmp(f.last_hop_mac_address, relay->mac(), 6), 0)
      << "last hop must be the relay node";
}

// Bug #6 + replay dedup: in a fully-connected triangle both sensors enroll
// concurrently (the master's relay queue must not starve one), and a's PIR
// event — which reaches the master both directly and re-relayed by b — is
// delivered to the hub exactly once (ReplayCache dedups the relayed copy).
TEST_F(MeshSimTest, NoDuplicateDeliveryInTriangleTopology) {
  addMaster();
  auto* a = addSensor(MAC_NODE_A);
  auto* b = addSensor(MAC_NODE_B);
  // Triangle: everyone hears everyone — replay/dedup must prevent duplicates
  world.bus.link(master, a);
  world.bus.link(master, b);
  world.bus.link(a, b);
  enroll(a);
  enroll(b);

  size_t before = hub->adapterDataFromOrigin(a->mac()).size();
  a->simulatePirMotion();
  runPolled(3000);
  auto frames = hub->adapterDataFromOrigin(a->mac());
  EXPECT_EQ(frames.size(), before + 1)
      << "exactly one copy must reach the hub (replay cache dedups the b-relayed copy)";
}

// Task 3 (Phase 2 multi-hop uplink plan): a relay one hop from the master must
// learn the master as a NeighborTable forwarding candidate from the master's
// own beacons, without any explicit routing action — this is what lets a later
// leaf pick the relay (and the relay pick the master) as next hop.
TEST_F(MeshSimTest, RelayLearnsNeighborFromMasterBeacon) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  world.bus.link(master, relay);
  enroll(relay);
  runPolled(4000); // let >=1 master beacon (3s interval) reach the relay

  bool eligible = false;
  relay->with([&](lattice::mesh::Mesh& m, auto*) {
    uint8_t out[6];
    // relay is distance 1 from master; a distance-0 neighbor (the master) must
    // be selectable as next hop for a hypothetical distance-1 sender.
    eligible = m.testNeighbors().selectNextHop(1, m.testMillisNow(), out);
    if (eligible)
      EXPECT_EQ(memcmp(out, master->mac(), 6), 0) << "master is the distance-0 neighbor";
    return 0;
  });
  EXPECT_TRUE(eligible) << "relay should have learned the master as a neighbor from its beacon";
}

// Phase 3 (downlink source routing, spec §4): the downlink counterpart of
// SensorOutOfMasterRangeRelaysThroughMiddleNode above. A leaf out of direct RF
// range of the master (chain: master -> relay -> leaf, leaf NOT linked to
// master) receives and applies a server-issued CONFIG_SET that the master had
// to seal with the leaf's k_down and source-route through the relay
// (Mesh::sendDownlinkToNode) — the relay cannot read the sealed payload, only
// forward it one hop further per the route_path header. The master only knows
// that route once the leaf's periodic route report (ROUTE_REPORT_INTERVAL_MS)
// has reached it (RouteTable, Task 5), so wait that out first; before then the
// only path is the sealed flood-fallback, which this test intentionally
// avoids exercising.
TEST_F(MeshSimTest, MasterSendsSealedConfigSetThroughRelayToLeaf) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);
  enroll(relay);
  enroll(leaf);

  // Let the leaf's periodic route report reach the master so it learns the
  // relay path (RouteTable) before the downlink is issued.
  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  auto typeOf = [](sim::SimNode* n) {
    return n->with(
        [](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) { return a->getAdapterType(); });
  };
  ASSERT_EQ(typeOf(leaf), lattice::adapter::PIR_ADAPTER) << "precondition: leaf starts as PIR";
  ASSERT_EQ(typeOf(relay), lattice::adapter::PIR_ADAPTER) << "precondition: relay starts as PIR";

  hub->sendConfigSet(leaf->mac(), lattice::adapter::SERIAL_ADAPTER);
  runPolled(5000); // master seals+source-routes -> relay forwards -> leaf opens+applies+reboots

  EXPECT_EQ(typeOf(leaf), lattice::adapter::SERIAL_ADAPTER)
      << "leaf must open the sealed downlink with its k_down and apply the CONFIG_SET despite "
         "being 2 hops from the master";
  EXPECT_EQ(typeOf(relay), lattice::adapter::PIR_ADAPTER)
      << "a relay forwarding a downlink addressed to another node must not apply it itself";
  EXPECT_FALSE(relay->restartRequested())
      << "the relay must never locally deliver a downlink addressed to the leaf";
}
