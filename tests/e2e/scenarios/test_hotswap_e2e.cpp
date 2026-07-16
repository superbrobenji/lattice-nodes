// Task 13: adapter hotswap via OP_CONFIG_SET.
//
// The core apply+persist+config-reboot path (PIR -> SERIAL) is covered by
// ServerBroadcastTest.ConfigSetReachesNodeAdapterAndPersists
// (test_harness_smoke.cpp, Task 6a). These scenarios add the two angles that
// test does not: a config-set addressed to a DIFFERENT node is ignored, and a
// hot-swapped type survives a later independent power cycle.
//
// AdapterFactory builds only PIR_ADAPTER and SERIAL_ADAPTER (others err::fail
// "Unknown adapter type"), so the only swap target is SERIAL_ADAPTER.
#include "harness/MeshSimTest.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/Adapter.h"

namespace {
lattice::adapter::adapter_types typeOf(sim::SimNode* n) {
  return n->with(
      [](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) { return a->getAdapterType(); });
}
} // namespace

// Config-set targeted at another node must not swap this node's adapter and
// must not trigger its reboot.
TEST_F(MeshSimTest, ConfigSetForOtherNodeIsIgnored) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A); // boots as PIR
  world.bus.link(master, sensor);
  enroll(sensor);
  ASSERT_EQ(typeOf(sensor), lattice::adapter::PIR_ADAPTER);

  hub->sendConfigSet(MAC_NODE_B, lattice::adapter::SERIAL_ADAPTER); // different, absent node
  runPolled(5000);

  EXPECT_EQ(typeOf(sensor), lattice::adapter::PIR_ADAPTER)
      << "a config-set targeted at another node must not swap this node's adapter";
  EXPECT_FALSE(sensor->restartRequested())
      << "a config-set for another node must not trigger this node's reboot";
}

// Hot-swapped adapter type persists across a subsequent plain power cycle.
// (Enabled once no-route transients stopped escalating to err::fail — a
// hot-swapped non-master SERIAL node's first post-reboot health report finds no
// route yet and is now dropped quietly instead of tripping the error path.)
TEST_F(MeshSimTest, HotSwappedAdapterTypeSurvivesPlainReboot) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A); // boots as PIR
  world.bus.link(master, sensor);
  enroll(sensor);
  ASSERT_EQ(typeOf(sensor), lattice::adapter::PIR_ADAPTER);

  hub->sendConfigSet(sensor->mac(), lattice::adapter::SERIAL_ADAPTER);
  runPolled(5000); // config broadcast -> sensor saves EEPROM + ESP.restart -> auto-reboot
  ASSERT_EQ(typeOf(sensor), lattice::adapter::SERIAL_ADAPTER)
      << "config-set must swap the running adapter after the config reboot";

  sensor->reboot(); // independent power cycle
  EXPECT_EQ(typeOf(sensor), lattice::adapter::SERIAL_ADAPTER)
      << "swapped adapter type must persist in EEPROM across a plain reboot";
}
