// Task 12: dual-master e2e scenarios.
//
// In dual-master mode a node TOFU-learns a SECOND master's beacon as its
// secondary, and (route-wise) adopts whichever master is currently beaconing.
// Both scenarios below are enabled: route-adoption failover works and is
// tested. Full DATA failover does not work — a node enrolls with, and is
// ECDH-keyed to, only ONE master, so it has no encrypted link to the secondary;
// its post-failover uplink is dropped (quietly, since #9). That keying gap is
// documented in docs/design-gaps/multihop-data-uplink.md ("Related gap:
// dual-master data failover"); these tests assert route adoption, not delivery.
#include "harness/MeshSimTest.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/Adapter.h"
#include "project_config.h"

class DualMasterTest : public MeshSimTest {
protected:
  static constexpr uint8_t MAC_MASTER2[6] = {0x02, 0, 0, 0, 0, 0x02};

  static void setDual(sim::SimNode* n) {
    n->with([](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
      m.setDualMasterMode(true);
      return 0;
    });
  }

  // Enroll `sensor` with the primary (only it up), then bring up a secondary
  // master in dual-master mode and let the sensor TOFU-learn it. Returns the
  // secondary node.
  sim::SimNode* bringUpSecondary(sim::SimNode* sensor) {
    setDual(master);
    setDual(sensor);
    sim::NodeConfig m2{};
    memcpy(m2.mac, MAC_MASTER2, 6);
    m2.isMaster = true;
    m2.adapterType = lattice::adapter::SERIAL_ADAPTER;
    sim::SimNode* master2 = world.addNode(m2);
    setDual(master2);
    world.bus.link(master2, sensor);
    world.bus.link(master, master2);
    runPolled(6000);
    return master2;
  }
};

// Route-adoption failover: when the primary goes silent, the node adopts the
// secondary as currentMaster. This asserts route adoption only, which works.
// DATA delivery to the secondary still needs the node to be ECDH-keyed to it
// (it enrolled with the primary only) — the dual-master facet of the keying gap
// in docs/design-gaps/multihop-data-uplink.md — but that is a separate concern
// this test does not exercise. (Enabled once no-route transients stopped
// escalating to err::fail; the node's post-failover uplink to the unkeyed
// secondary is now a quiet drop rather than an error.)
TEST_F(DualMasterTest, FailsOverToSecondaryMasterWhenPrimaryGoesSilent) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  auto* master2 = bringUpSecondary(sensor);

  world.bus.unlink(master, sensor); // primary goes silent to the sensor
  runPolled(lattice::config::STALE_MASTER_THRESHOLD_MS + 6000);

  bool onSecondary = sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master2->mac(), 6) == 0;
  });
  EXPECT_TRUE(onSecondary) << "after the primary goes silent, the node must adopt the secondary";
}

// The implemented, testable part: in dual-master mode a node TOFU-learns the
// secondary master from its beacon (prerequisite for any failover). No uplink
// to the secondary is attempted here, so no keying gap is hit.
TEST_F(DualMasterTest, LearnsSecondaryMasterInDualMode) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  auto* master2 = bringUpSecondary(sensor);

  bool knowsSecondary = sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return m.enrollment.hasMasterMacSecondary &&
           memcmp(m.enrollment.knownMasterMacSecondary, master2->mac(), 6) == 0;
  });
  EXPECT_TRUE(knowsSecondary) << "sensor must TOFU-learn the secondary master in dual-master mode";
}

// Full DATA failover (closes gap #8): the server designates a secondary master
// at enrollment time (JOIN_ACK's secondary_master_mac/secondary_public_key,
// Phase 4 tasks 1-2), so the sensor is ECDH-keyed to BOTH masters from the
// start. When the primary goes silent and the sensor adopts the secondary as
// currentMaster, its post-failover uplink must actually DECRYPT there — not
// just arrive. FakeHub only surfaces what the master's serial bridge decoded
// AFTER Mesh opened the sealed payload with peerE2EKeys(origin) (see
// Mesh.cpp's "needsOpen" uplink-open path); if the master couldn't open the
// frame, it is dropped and never reaches serial/FakeHub at all. So arrival at
// hub2 is itself proof of decryption at master2, not just of routing.
TEST_F(DualMasterTest, UplinkReachesSecondaryMasterAfterFailover) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  auto* master2 = bringUpSecondary(sensor); // dual mode on; master2 linked to sensor + primary

  // Read master2's pubkey (server knows both masters' identities).
  uint8_t m2Pub[32];
  master2->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    memcpy(m2Pub, m.enrollment.getPublicKey(), 32);
    return 0;
  });

  // Enroll the sensor with the PRIMARY, server designating master2 as the secondary.
  runPolled(11000);
  const mesh_message* req = hub->enrollmentFrom(sensor->mac());
  ASSERT_NE(req, nullptr) << "enrollment request never reached hub";
  hub->approveEnrollment(sensor->mac(), req->enrollment_public_key, master2->mac(), m2Pub);
  runPolled(5000);
  ASSERT_TRUE(sensor->with(
      [&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.isEnrolled(); }));

  // Server sync: teach master2 the sensor's pubkey (so master2 can open its uplink).
  uint8_t sensorPub[32];
  sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    memcpy(sensorPub, m.enrollment.getPublicKey(), 32);
    return 0;
  });
  master2->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    m.enrollPeer(sensor->mac(), sensorPub);
    return 0;
  });

  // Attach a hub to master2 so we can observe what reaches it.
  sim::FakeHub hub2(master2);

  // Primary goes silent → sensor fails over to master2.
  world.bus.unlink(master, sensor);
  runPolled(lattice::config::STALE_MASTER_THRESHOLD_MS + 4000);
  ASSERT_TRUE(sensor->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master2->mac(), 6) == 0;
  })) << "sensor must adopt the secondary as currentMaster once the primary goes stale";

  // Sensor sends PIR data; it must arrive DECRYPTED at master2's hub.
  size_t before = hub2.adapterDataFromOrigin(sensor->mac()).size();
  sensor->simulatePirMotion();
  runPolled(4000);
  hub2.poll();
  auto frames = hub2.adapterDataFromOrigin(sensor->mac());
  ASSERT_GT(frames.size(), before) << "post-failover uplink reached the secondary";
  // FakeHub delivers what the master OPENED — so arrival proves master2 decrypted it
  // with the sensor's k_up derived against the (sensor, master2) ECDH pair.
}
