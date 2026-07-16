// Task 8 / 8b: PIR data flow e2e scenarios. Confirms a PIR sensor's motion
// event travels sensor -> master -> serial -> hub as a MESH_TYPE_ADAPTER_DATA
// frame with the sensor's own MAC preserved as origin and data_type
// PIR_ADAPTER, and that the adapter's cooldown suppresses a rapid re-trigger.
//
// hop_count convention (verified against firmware, see Task 8b report):
// Mesh::buildMessage stamps hop_count = 0 at origination and it is incremented
// ONLY when an intermediate node RELAYS the frame onward (Mesh::processAdapterData
// uplink branch; locked in by test_mesh_logic AdapterDataRelayTest, which sees
// 1 -> 2 on relay). The terminal master delivers locally WITHOUT incrementing.
// So a direct sensor->master link yields hop_count == 0 at the hub (the brief's
// "== 1" guess reflected a hops-travelled model the firmware does not use). A
// two-hop sensor->relay->master path would arrive as hop_count == 1, etc.
//
// Cooldown: PirAdapter::_cooldownSeconds defaults to 3 (main/src/adapter/pir/
// PirAdapter.cpp ctor), i.e. a 3000 ms window during which the interrupt stays
// detached and re-triggers are ignored. The 500 ms windows below place the
// second trigger firmly inside that window.
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST_F(MeshSimTest, PirMotionReachesHubWithCorrectOriginAndType) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();
  sensor->simulatePirMotion();
  runPolled(2000);

  auto frames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GT(frames.size(), before);
  const mesh_message& f = frames.back();
  EXPECT_EQ(f.data_type, lattice::adapter::PIR_ADAPTER);
  EXPECT_EQ(memcmp(f.origin_mac_address, sensor->mac(), 6), 0);
  EXPECT_EQ(f.hop_count, 0) << "direct sensor -> master: 0 relay hops (origin=0, "
                               "master delivers locally without incrementing)";
}

TEST_F(MeshSimTest, PirCooldownSuppressesRapidRetrigger) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  runPolled(500);
  size_t afterFirst = hub->adapterDataFromOrigin(sensor->mac()).size();
  ASSERT_GT(afterFirst, 0u) << "first motion must produce a frame at the hub "
                               "(otherwise the cooldown assertion is vacuous)";

  sensor->simulatePirMotion(); // within the 3000 ms cooldown window
  runPolled(500);
  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), afterFirst)
      << "PIR cooldown must suppress the second trigger";
}
