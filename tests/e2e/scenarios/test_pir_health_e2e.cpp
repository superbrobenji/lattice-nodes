// Task 14a: PIR node health telemetry reaches the hub.
//
// A non-master PIR node emits OP_NODE_HEALTH every HEALTH_REPORT_INTERVAL_MS;
// it travels node -> master -> serial -> hub as adapter data.
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "project_config.h"

TEST_F(MeshSimTest, PirNodeHealthReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  runPolled(lattice::config::HEALTH_REPORT_INTERVAL_MS + 5000);

  bool sawNodeHealth = false;
  for (const auto& m : hub->received) {
    if (m.data[0] == OP_NODE_HEALTH && memcmp(&m.data[2], sensor->mac(), 6) == 0)
      sawNodeHealth = true;
  }
  EXPECT_TRUE(sawNodeHealth) << "the PIR node's OP_NODE_HEALTH must reach the hub";
}
