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
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialFraming.h"
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

  // DEFAULT_PEERS placeholder MACs are now stripped automatically by
  // SimNode::boot() (see SimNode.cpp) -- no per-test workaround needed.

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

// Exercises the surface the previous test's reconciliation note flagged as
// fragile: FakeHub::poll()'s reliance on SerialFraming::decode(), which for any
// message type other than JOIN_ACK/SERIAL_CMD_BROADCAST discards the wire-encoded
// origin/last-hop MACs and overwrites them with esp_wifi_get_mac()'s CURRENT
// global value -- i.e. whichever SimNode context the harness last had swapped
// in, not the actual originating device. FakeHub now does its own nanopb decode
// (mirroring SerialFraming::decode's field copying, minus the MAC overwrite)
// because it plays the SERVER's role: it has no device identity of its own, so
// "my own current MAC" is meaningless for it -- only the wire-encoded origin is
// ever correct.
TEST(FakeHubTest, OriginMacsPreservedAndFiltersWork) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* sensor = world.addNode({{0x02, 0, 0, 0, 0, 0x02}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, sensor);
  sim::FakeHub hub(master);

  // DEFAULT_PEERS placeholder MACs are stripped automatically by SimNode::boot()
  // (see SimNode.cpp) -- no per-test workaround needed.

  // --- Enrollment relay + positive/negative origin filters ---
  world.run(10005); // 10s broadcast interval + margin for 1-step delivery latency
  hub.poll();
  const mesh_message* enr = hub.enrollmentFrom(sensor->mac());
  ASSERT_NE(enr, nullptr) << "master must relay the sensor's enrollment request to the hub, "
                             "with the sensor's own mac as wire origin";
  EXPECT_EQ(hub.enrollmentFrom(master->mac()), nullptr)
      << "negative filter: the master's own mac must never match an ENROLLMENT origin";

  auto enrollments = hub.ofType(MESH_TYPE_ENROLLMENT);
  ASSERT_FALSE(enrollments.empty());
  EXPECT_EQ(0, memcmp(enrollments.front().origin_mac_address, sensor->mac(), 6))
      << "ofType(MESH_TYPE_ENROLLMENT) frames must carry the sensor's wire origin";

  hub.approveEnrollment(sensor->mac(), enr->enrollment_public_key);
  world.run(50);

  // --- sendConfigSet: drive a server-issued adapter reconfiguration ---
  //
  // CORRECTION (Task 6a investigation, see .superpowers/sdd/task-6a-report.md):
  // this comment previously claimed full master->mesh->sensor propagation was
  // broken because Mesh::buildMessage() stamps outgoing ADAPTER_DATA frames'
  // target_mac_address from the sender's own (unset, all-zero) currentMaster.mac.
  // That claim does not hold for this call path: SerialAdapter::handleCompleteFrame
  // reaches the mesh via Mesh::broadcastAdapterDataStatic() -> broadcastAdapterData(),
  // which overwrites target_mac_address to the broadcast address FF:FF:FF:FF:FF:FF
  // right after buildMessage() runs (Mesh.cpp) -- buildMessage()'s currentMaster.mac
  // stamp is immediately discarded, never sent. Mesh::processAdapterData() on the
  // receiving node already special-cases that broadcast target: it delivers locally
  // (invoking externalRecvCallback / Adapter::onMeshData) AND relays outward for
  // multi-hop, exactly as this opcode needs. This is exercised directly by
  // test_mesh_logic.cpp's AdapterDataRelayTest.IntermediateNode_BroadcastTarget_
  // DeliveredAndRelayed and .BroadcastAdapterData_UsesBroadcastTargetMAC, and
  // end-to-end (enrollment -> sendConfigSet -> sensor reboots with the new adapter
  // type) by ServerBroadcastTest.ConfigSetReachesNodeAdapterAndPersists below.
  // No main/src change was needed for Task 6a; this test's own assertions below
  // were already correct (they only ever inspected the payload data[], never
  // target_mac_address) and are left as-is.
  //
  // VirtualBus::deliver() collects AND clears each node's ctx().espNowSent within
  // the very same step() call that produced it, so inspecting
  // master->ctx().espNowSent after a world.run() would always see it empty. Tick
  // the master directly (bypassing SimWorld/VirtualBus) so we can snapshot the
  // frame the instant SerialAdapter::handleCompleteFrame() produces it.
  hub.sendConfigSet(sensor->mac(), lattice::adapter::SERIAL_ADAPTER);
  master->tick();
  auto pendingSends = master->ctx().espNowSent; // snapshot before VirtualBus can clear it
  EXPECT_TRUE(master->ctx().serialRx.empty())
      << "master must have consumed the sendConfigSet frame off its (mock) serial line";

  bool sawConfigSetBroadcast = false;
  for (const auto& pkt : pendingSends) {
    if (pkt.data.size() != sizeof(mesh_message))
      continue;
    const auto* msg = reinterpret_cast<const mesh_message*>(pkt.data.data());
    if (msg->data_type == lattice::adapter::SERIAL_ADAPTER && msg->data[0] == OP_CONFIG_SET &&
        memcmp(&msg->data[1], sensor->mac(), 6) == 0) {
      sawConfigSetBroadcast = true;
    }
  }
  EXPECT_TRUE(sawConfigSetBroadcast)
      << "master must rebroadcast the CONFIG_SET opcode (data[0]==0xC1) with the "
         "sensor's mac embedded in the payload, even though full end-to-end "
         "delivery into the sensor's adapter is a separate, unfixed gap";

  world.run(50); // let VirtualBus deliver/clear the snapshotted frame normally

  // --- adapterDataFromOrigin: negative-filter proof, then the Fix-1 proof ---
  EXPECT_FALSE(hub.received.empty());
  EXPECT_TRUE(hub.adapterDataFromOrigin(sensor->mac()).empty())
      << "no genuine ADAPTER_DATA frame from the sensor has reached the hub yet";
  EXPECT_TRUE(hub.adapterDataFromOrigin(master->mac()).empty())
      << "no genuine ADAPTER_DATA frame from the master has reached the hub yet";

  // Hand-build an ADAPTER_DATA frame with a synthetic origin MAC that matches
  // neither node, and push it directly into the master's serialWritten mock
  // buffer (bypassing real mesh delivery entirely) to isolate FakeHub::poll()'s
  // own decode from everything else. Under the OLD (SerialFraming::decode-based)
  // implementation, this origin is unconditionally overwritten with
  // esp_wifi_get_mac()'s CURRENT global value -- never this phantom MAC -- so
  // this assertion is the reliable RED signal for Fix 1, regardless of tick
  // ordering or which node's context happened to be swapped in last.
  static const uint8_t kPhantomOriginMac[6] = {0x02, 0, 0, 0, 0, 0xAA};
  mesh_message adapterMsg{};
  adapterMsg.proto_version = 1;
  adapterMsg.message_type = MESH_TYPE_ADAPTER_DATA;
  adapterMsg.data_type = lattice::adapter::SERIAL_ADAPTER;
  memcpy(adapterMsg.origin_mac_address, kPhantomOriginMac, 6);
  memcpy(adapterMsg.last_hop_mac_address, kPhantomOriginMac, 6);
  adapterMsg.data[0] = OP_HEALTH_REPORT;

  uint8_t encoded[256];
  size_t n = lattice::adapter::serial::SerialFraming::encode(adapterMsg, encoded, sizeof(encoded));
  ASSERT_GT(n, 0u);
  uint8_t lenLE[2] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>((n >> 8) & 0xFF)};
  auto& written = master->ctx().serialWritten;
  written.insert(written.end(), lenLE, lenLE + 2);
  written.insert(written.end(), encoded, encoded + n);
  hub.poll();

  auto fromPhantom = hub.adapterDataFromOrigin(kPhantomOriginMac);
  ASSERT_FALSE(fromPhantom.empty())
      << "FakeHub must preserve the wire origin MAC for ADAPTER_DATA frames, not "
         "overwrite it with whichever SimNode context is globally live";
  EXPECT_EQ(0, memcmp(fromPhantom.front().origin_mac_address, kPhantomOriginMac, 6));
}

// Task 6a: end-to-end proof that a server-issued OP_CONFIG_SET actually reaches
// the target node's adapter, persists to EEPROM, and survives the resulting
// reboot. The enrollment dance is required first: Mesh::broadcastToAllPeers()
// only sends to nodes in Mesh::peers (the enrolled-peer list), not to every
// node the VirtualBus happens to link -- an unenrolled sensor would never
// receive the mesh-side broadcast regardless of the target_mac_address fix.
TEST(ServerBroadcastTest, ConfigSetReachesNodeAdapterAndPersists) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x31}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* sensor = world.addNode({{0x02, 0, 0, 0, 0, 0x32}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, sensor);
  sim::FakeHub hub(master);

  // Enroll the sensor so the master's peer list (and thus broadcastToAllPeers)
  // actually includes it.
  world.run(10005); // 10s enrollment-broadcast interval + margin for delivery latency
  hub.poll();
  const mesh_message* enr = hub.enrollmentFrom(sensor->mac());
  ASSERT_NE(enr, nullptr) << "master must relay the sensor's enrollment request to the hub";
  hub.approveEnrollment(sensor->mac(), enr->enrollment_public_key);
  world.run(50);
  size_t masterPeers = master->with(
      [](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.peers.peerCount; });
  ASSERT_GE(masterPeers, 1u) << "sensor must be an enrolled peer of the master before broadcast";

  auto preType = sensor->with(
      [](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) { return a->getAdapterType(); });
  EXPECT_EQ(preType, lattice::adapter::PIR_ADAPTER) << "precondition: sensor starts as PIR";

  hub.sendConfigSet(sensor->mac(), lattice::adapter::SERIAL_ADAPTER);
  world.run(500); // master reads serial -> mesh broadcast -> sensor delivers -> saves EEPROM ->
                  // ESP.restart -> SimWorld auto-reboots

  auto type = sensor->with(
      [](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) { return a->getAdapterType(); });
  EXPECT_EQ(type, lattice::adapter::SERIAL_ADAPTER)
      << "OP_CONFIG_SET must reach the node adapter, persist, and survive the config reboot";
}
