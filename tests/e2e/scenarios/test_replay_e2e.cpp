// Task 11: replay-protection e2e scenarios.
//
// ReplayCache dedups on (origin_mac, epoch_num, seq_num). A frame the master
// has already processed must be dropped if re-injected; a reboot bumps the
// node's boot epoch so its post-reboot traffic (new epoch, low seq) is fresh
// and accepted.
//
// Capture note: the brief's original approach polled sensor->ctx().espNowSent
// after world.step(), but SimWorld::step ticks nodes THEN calls
// bus.deliver(), which drains and clears espNowSent every step — so that
// buffer is empty by the time step() returns. Instead we take the exact frame
// the master already saw from the hub's decoded copy (FakeHub preserves the
// wire origin/epoch/seq), reconstruct the raw mesh_message, and re-inject it at
// the master. That is precisely the tuple ReplayCache recorded on first
// delivery, so it is the correct replay to test.
#include "harness/MeshSimTest.h"
#include <cstring>

TEST_F(MeshSimTest, ReplayedFrameIsDropped) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  runPolled(2000);

  auto legitFrames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GE(legitFrames.size(), 1u) << "the genuine motion frame must reach the hub first";
  size_t legit = legitFrames.size();

  // Reconstruct the raw frame the master already processed (same origin/epoch/seq)
  // and replay it directly at the master.
  mesh_message replayed = legitFrames.back();
  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), reinterpret_cast<const uint8_t*>(&replayed), sizeof(replayed));
  sim::swapOut(master->ctx());
  runPolled(2000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), legit)
      << "replayed (origin,epoch,seq) must be dropped by ReplayCache, not re-forwarded";

  // Positive control (guards against a vacuous pass): the SAME injection path
  // with a fresh seq must be forwarded — proving the replay above was dropped
  // by dedup specifically, not silently ignored for some unrelated reason.
  mesh_message fresh = replayed;
  fresh.seq_num = static_cast<uint16_t>(replayed.seq_num + 1);
  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), reinterpret_cast<const uint8_t*>(&fresh), sizeof(fresh));
  sim::swapOut(master->ctx());
  runPolled(2000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), legit + 1)
      << "a fresh-seq frame via the same path must be forwarded (injection path works)";
}

TEST_F(MeshSimTest, RebootBumpsEpochSoNewTrafficIsAccepted) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  runPolled(2000);
  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();
  ASSERT_GE(before, 1u);

  sensor->reboot(); // boot epoch increments (persisted BOOT_EPOCH + 1); tx seq resets
  runPolled(2000);  // re-sync: hear a beacon, re-establish route to master
  sensor->simulatePirMotion();
  runPolled(3000);

  EXPECT_GT(hub->adapterDataFromOrigin(sensor->mac()).size(), before)
      << "post-reboot traffic (higher epoch, low seq) must be accepted, not mistaken for replay";
}
