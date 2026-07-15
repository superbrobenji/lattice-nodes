// Regression coverage for Task 6b: the master's own health telemetry must
// reach its own serial port (the server). Before the fix, Mesh::transmit()'s
// master branch routed ALL self-originated adapter data through
// broadcastAdapterData() -> broadcastToAllPeers(), which sends to OTHER mesh
// peers and explicitly skips self -- so the master never answered its own
// OP_HEALTH_REQ, and its periodic health report never left the device.
//
// Master + FakeHub only: no other mesh nodes needed to exercise this path.
#include <cstring>
#include <gtest/gtest.h>
#include "harness/FakeHub.h"
#include "harness/SimWorld.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "project_config.h"

TEST(MasterHealthTest, HealthReqGetsAnsweredOverSerial) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  sim::FakeHub hub(master);

  world.run(50); // drain any boot-time frames
  hub.poll();
  size_t before = hub.received.size();

  hub.sendHealthReq();
  world.run(200);
  hub.poll();

  bool found = false;
  for (const auto& m : hub.received) {
    if (m.data[0] == OP_HEALTH_REPORT && memcmp(&m.data[2], master->mac(), 6) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "master must answer OP_HEALTH_REQ on its own serial";
  EXPECT_GT(hub.received.size(), before);
}

TEST(MasterHealthTest, PeriodicHealthReportReachesHub) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  sim::FakeHub hub(master);

  // Runtime note: ticks ~32k virtual ms with a single node -- acceptable
  // wall-clock cost (see task report for measured time).
  world.run(lattice::config::HEALTH_REPORT_INTERVAL_MS + 2000);
  hub.poll();

  bool found = false;
  for (const auto& m : hub.received) {
    if (m.data[0] == OP_HEALTH_REPORT && memcmp(&m.data[2], master->mac(), 6) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "master's periodic health report must reach the hub over its own serial";
}
