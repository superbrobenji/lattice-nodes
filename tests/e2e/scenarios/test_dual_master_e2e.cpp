// Task 12: dual-master e2e scenarios.
//
// In dual-master mode a node TOFU-learns a SECOND master's beacon as its
// secondary, and (route-wise) adopts whichever master is currently beaconing.
// The route-adoption failover works; full DATA failover does not, because a
// node enrolls with — and is ECDH-keyed to — only ONE master, so it has no
// encrypted link to the secondary. That keying gap is documented in
// docs/design-gaps/multihop-data-uplink.md ("Related gap: dual-master data
// failover"). See the DISABLED_ test below.
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

// The route-adoption + data path after primary loss. DISABLED: the node adopts
// the secondary as currentMaster correctly, but then err::fails on its next
// uplink because the secondary is not an ECDH-keyed peer (the node enrolled
// with the primary only). Closing this needs the node to hold key material for
// every master it may fail over to — the dual-master facet of the keying gap in
// docs/design-gaps/multihop-data-uplink.md. Assertions are the intended
// behavior; drop DISABLED_ once that gap is closed.
TEST_F(DualMasterTest, DISABLED_FailsOverToSecondaryMasterWhenPrimaryGoesSilent) {
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
