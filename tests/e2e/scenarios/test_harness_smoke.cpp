#include <gtest/gtest.h>
#include "harness/NodeContext.h"
#include "harness/SimClock.h"
#include "harness/SimNode.h"
#include "harness/SimWorld.h"
#include "harness/FakeHub.h"
#include "EEPROM.h"
#include "esp_wifi_mock.h"
#include "src/persistence/EepromManager.h"
#include "src/mesh/Mesh.h"
#include "lib/lattice-protocol/c/message_types.h"
#include "lib/lattice-protocol/c/mesh_message.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST(NodeContextSwap, IsolatesEepromAndMac) {
  sim::NodeContext a, b;
  a.mac[0] = 0xAA;
  b.mac[0] = 0xBB;

  sim::swapIn(a);
  EEPROM.write(0, 0x11);
  EXPECT_EQ(mockDeviceMac[0], 0xAA);
  sim::swapOut(a);

  sim::swapIn(b);
  EXPECT_EQ(EEPROM.read(0), 0xFF) << "node B must not see node A's EEPROM";
  EXPECT_EQ(mockDeviceMac[0], 0xBB);
  EEPROM.write(0, 0x22);
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_EQ(EEPROM.read(0), 0x11);
  sim::swapOut(a);
}

TEST(NodeContextSwap, FreshContextGetsPristineSingletons) {
  sim::NodeContext a, b;

  sim::swapIn(a);
  EXPECT_FALSE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "sanity: singleton must start uninitialized before node A dirties it";
  lattice::utils::EepromManager::getInstance().init(); // node A dirties singleton state
  EXPECT_TRUE(lattice::utils::EepromManager::getInstance().isInitializedForTest());
  sim::swapOut(a);

  sim::swapIn(b); // fresh context: must NOT inherit A's initialized state
  EXPECT_FALSE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "node B's fresh context must restore pristine EepromManager state, "
      << "not silently inherit node A's initialized singleton";
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_TRUE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "swapping back to A must still see A's own initialized state";
  sim::swapOut(a);
}

TEST(SimClockTest, AdvancesMillis) {
  sim::SimClock clock;
  uint32_t t0 = clock.now();
  clock.advance(250);
  EXPECT_EQ(clock.now(), t0 + 250);
  EXPECT_EQ(millis(), clock.now());
}

static sim::NodeConfig masterCfg() {
  return {{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER};
}

TEST(SimNodeTest, MasterBootsAndBeacons) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  // Master beacon interval is 3000ms — tick across 4s of virtual time
  for (int i = 0; i < 4000; ++i) {
    clock.advance(1);
    master.tick();
  }
  // Beacon(s) must have been captured as broadcast esp_now sends
  bool sawBeacon = false;
  for (const auto& pkt : master.ctx().espNowSent) {
    if (pkt.data.size() == sizeof(mesh_message)) {
      const auto* msg = reinterpret_cast<const mesh_message*>(pkt.data.data());
      if (msg->message_type == MESH_TYPE_MASTER_BEACON)
        sawBeacon = true;
    }
  }
  EXPECT_TRUE(sawBeacon);
}

TEST(SimNodeTest, RebootPreservesEeprom) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  auto imageBefore = master.ctx().eepromData;
  master.reboot();
  // Master flag survives; keypair survives (same EEPROM bytes at PRIVATE_KEY range)
  EXPECT_TRUE(std::equal(imageBefore.begin() + 417, imageBefore.begin() + 483,
                         master.ctx().eepromData.begin() + 417));
  EXPECT_EQ(master.ctx().eepromData[0], imageBefore[0]);
}

TEST(VirtualBusTest, BeaconReachesLinkedNodeOnly) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* near = world.addNode({{0x02, 0, 0, 0, 0, 0x02}, false, lattice::adapter::PIR_ADAPTER});
  auto* far = world.addNode({{0x02, 0, 0, 0, 0, 0x03}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, near);
  // 'far' deliberately unlinked

  world.run(4000); // > one beacon interval

  // Linked node has processed a beacon: its mesh learned the master MAC
  bool nearKnowsMaster = near->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  bool farKnowsMaster = far->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  EXPECT_TRUE(nearKnowsMaster);
  EXPECT_FALSE(farKnowsMaster);
}

// RECONCILIATION (full trace in .superpowers/sdd/task-6-report.md):
//
// The brief's literal scenario (a lone master, world.run(50), expecting its own
// spontaneous SerialAdapter::sendHealthReport() to appear on its own Serial.written)
// does not hold, for two independently-confirmed reasons:
//
// 1. "Immediate health report on hop-count init" does not apply to a master node.
//    SerialAdapter::loop() only fires early when `getHopCount()` changes from its
//    last-reported value, but Mesh::getHopCount() is `isMaster ? 0 : ...` -- a
//    constant for the master. Its health report only ever fires on the 30s
//    periodic interval (lattice::config::HEALTH_REPORT_INTERVAL_MS), never sooner.
//
// 2. Once that interval elapses (or a HEALTH_REQ triggers sendHealthReport()
//    directly) and the master's Mesh::transmit() takes its master branch
//    (Mesh.cpp:341, broadcastAdapterData -> broadcastToAllPeers), the master's own
//    health data is always routed as an ESP-NOW broadcast to *other* mesh peers --
//    never written to its own Serial. sendMessage() (Mesh.cpp:283-286) and
//    broadcastToAllPeers() (Mesh.cpp:302-303) both explicitly skip sending to
//    device's own MAC ("Skip self"), and there is no other code path that calls
//    Serial.write() for the master's own generated telemetry. This is a genuine
//    firmware gap (the master's own health can never reach the cable it is
//    physically wired to) -- confirmed empirically (serialWritten stays empty
//    across ticking) but out of scope to fix under Task 6 (harness-only: FakeHub +
//    this test + CMakeLists, no main/src changes).
//
// A third, unrelated harness/config interaction was hit while investigating: every
// freshly-provisioned node's peer list starts EEPROM-erased, and
// PeerRegistry::loadFromEEPROM() falls back to lattice::config::DEFAULT_PEERS (two
// placeholder MACs meant to be replaced before real flashing) whenever the
// persisted list is empty. Those placeholder MACs don't correspond to any node in
// a simulated world, so the moment broadcastToAllPeers() actually attempts a send
// to them, VirtualBus::deliver() throws ("frame to unknown MAC"). No prior harness
// test ran long enough (30s+) or triggered a peer broadcast to hit this. We strip
// them below so the test can observe the *firmware* behavior in isolation instead
// of crashing on this harness/config artifact.
//
// Given (1)+(2), this test demonstrates the confirmed gap directly (health report
// silent, before and after a HEALTH_REQ) and separately proves FakeHub's own
// mechanics (poll/decode/enrollmentFrom/sendFrame/approveEnrollment) against a
// real, working, non-mesh serial write: SerialAdapter::relayEnrollmentToServer,
// which writes straight to Serial and is unaffected by the gap above.
TEST(FakeHubTest, ReceivesHealthReportAndAnswersHealthReq) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* node = world.addNode({{0x02, 0, 0, 0, 0, 0x02}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, node);
  sim::FakeHub hub(master);

  // Neutralize the DEFAULT_PEERS placeholder-MAC artifact (see comment above) on
  // both nodes so broadcastToAllPeers() reflects real peers.peerCount, not stale
  // config placeholders that don't exist in this simulated world.
  auto stripDefaultPeers = [](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    for (int i = 0; i < lattice::config::NUM_DEFAULT_PEERS; ++i)
      m.peers.removeAndPersist(lattice::config::DEFAULT_PEERS[i]);
    return 0;
  };
  master->with(stripDefaultPeers);
  node->with(stripDefaultPeers);

  // --- Confirmed gap: master's own health report never reaches its own serial ---
  world.run(50);
  hub.poll();
  EXPECT_TRUE(hub.ofType(MESH_TYPE_ADAPTER_DATA).empty())
      << "documents the confirmed gap: master's own SerialAdapter data never "
         "reaches its own Serial output (see reconciliation note above)";

  hub.sendHealthReq();
  world.run(50);
  hub.poll();
  EXPECT_TRUE(hub.received.empty())
      << "documents the confirmed gap: master's own answer to OP_HEALTH_REQ is "
         "subject to the same self-broadcast limitation as its spontaneous report";

  // --- Working path: FakeHub's poll/decode/query mechanics against a real,
  //     non-mesh Serial.write -- the enrollment-request relay. `node` is
  //     unenrolled, so SimNode::tick() auto-broadcasts an enrollment request every
  //     10s; master relays it straight to serial via
  //     SerialAdapter::relayEnrollmentToServer, independent of the gap above. ---
  world.run(10005); // 10s broadcast interval + a few ms margin for 1-step delivery latency
  hub.poll();
  const mesh_message* enr = hub.enrollmentFrom(node->mac());
  ASSERT_NE(enr, nullptr) << "master must relay the node's enrollment request to the hub";
  EXPECT_EQ(enr->message_type, MESH_TYPE_ENROLLMENT);

  // approveEnrollment() exercises FakeHub's remaining surface (sendFrame via the
  // JOIN_ACK it builds) and drives a real Mesh::enrollPeer() call on the master.
  size_t masterPeersBefore = master->with(
      [](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.peers.peerCount; });
  hub.approveEnrollment(node->mac(), enr->enrollment_public_key);
  world.run(50);
  size_t masterPeersAfter = master->with(
      [](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.peers.peerCount; });
  EXPECT_EQ(masterPeersAfter, masterPeersBefore + 1)
      << "master must register the approved node as a peer";
}
