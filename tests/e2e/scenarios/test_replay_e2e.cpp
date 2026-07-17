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
#include "mesh/Mesh.h"
#include <cstring>

TEST_F(MeshSimTest, ReplayedFrameIsDropped) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  // One step: PirAdapter::loop() detects the armed motion and sends. The bus's
  // deliver() call inside this step moves the send into VirtualBus's pending
  // queue but does NOT yet deliver it (delivery happens on the NEXT step) —
  // this is the one-step window where the sealed, in-flight frame can be
  // captured. See VirtualBus::lastPendingOfType's comment for why.
  world.step(1);

  // Task 6 (E2E AEAD): capture the SEALED frame exactly as it sits in flight
  // so replaying it preserves a valid auth tag. Reconstructing from the hub's
  // already-OPENED/plaintext copy (the old approach) would carry a stale tag
  // bound to the original ciphertext and fail auth for an unrelated reason,
  // making the "replay dropped" assertion below vacuous.
  mesh_message* pendingSealed = world.bus.lastPendingOfType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_NE(pendingSealed, nullptr) << "sensor must have a sealed ADAPTER_DATA uplink frame in flight";
  mesh_message sealed = *pendingSealed;

  runPolled(2000);

  auto legitFrames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GE(legitFrames.size(), 1u) << "the genuine motion frame must reach the hub first";
  size_t legit = legitFrames.size();

  // Replay the exact sealed bytes the master already processed (same origin/epoch/seq)
  // directly at the master.
  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), reinterpret_cast<const uint8_t*>(&sealed), sizeof(sealed));
  sim::swapOut(master->ctx());
  runPolled(2000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), legit)
      << "replayed (origin,epoch,seq) must be dropped by ReplayCache, not re-forwarded";

  // Positive control (guards against a vacuous pass): a genuinely fresh uplink
  // (new seq, freshly sealed) from the sensor must be forwarded — proving the
  // replay above was dropped by dedup specifically, not silently ignored for
  // some unrelated reason (e.g. a broken AEAD open). Drives the real Mesh
  // uplink path directly (buildMessage -> seal -> send), bypassing only
  // PirAdapter's cooldown gate, which is an adapter-layer concern unrelated to
  // mesh replay protection.
  sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    uint8_t data[64] = {1};
    mesh.transmitCore(lattice::adapter::PIR_ADAPTER, data, MESH_TYPE_ADAPTER_DATA);
    return 0;
  });
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
