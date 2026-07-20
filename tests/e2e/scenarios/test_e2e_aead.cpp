// Task 6: E2E AEAD integration — seal on uplink (Mesh::transmitCore), open at
// the master's local-delivery path (Mesh::processAdapterData /
// Mesh::processRouteReport). See harness/MeshSimTest.h for the shared
// fixture/enroll() helper this reuses verbatim.
//
// "On the bus" capture technique: a frame sent during simulation step N sits
// in VirtualBus's pending queue from the moment step N's deliver() call
// returns until step N+1's deliver() call actually delivers it (see
// VirtualBus::lastPendingOfType's doc comment in VirtualBus.h for why). That
// one-step window is where these scenarios inspect (Test 1) or tamper
// (Test 2) an in-flight, already-sealed frame before it reaches the master.
//
// PirAdapter::loop() detects an armed motion event and sends within the SAME
// loop() call (see PirAdapter.cpp), so a single world.step() after
// simulatePirMotion() is enough to get the sealed frame into flight.
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, SealedUplinkDeliversPlaintextToHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  world.step(1); // sends this step; now in flight, not yet delivered

  mesh_message* onWire = world.bus.lastPendingOfType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_NE(onWire, nullptr) << "sensor must have a sealed ADAPTER_DATA uplink frame in flight";
  // PirAdapter's motion frame is data[64] = {1} (no named OP_PIR_* opcode
  // constant exists in opcodes.h — PIR uses a bare literal). Once sealed,
  // ChaCha20-Poly1305 ciphertext occupying data[0] must not coincide with
  // that plaintext byte.
  EXPECT_NE(onWire->data[0], 1)
      << "the plaintext motion byte (data[0]==1) must not appear on the wire — "
         "the payload must be sealed in transit";

  runPolled(2000);

  auto frames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GE(frames.size(), 1u)
      << "the motion frame must still reach the hub once opened by the master";
  EXPECT_EQ(frames.back().data[0], 1)
      << "the hub must see the ORIGINAL plaintext opcode — the master must open the "
         "payload before local/serial delivery";
}

TEST_F(MeshSimTest, TamperedFrameIsDropped) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();

  sensor->simulatePirMotion();
  world.step(1);

  mesh_message* onWire = world.bus.lastPendingOfType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_NE(onWire, nullptr) << "sensor must have a sealed ADAPTER_DATA uplink frame in flight";
  onWire->data[0] ^= 0xFF; // flip a byte of the sealed ciphertext while in flight

  runPolled(2000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), before)
      << "a frame tampered in flight must fail AEAD auth at the master and never reach the hub";
}

TEST_F(MeshSimTest, ForgedFrameWithoutValidTagIsDropped) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  world.step(1);
  mesh_message* onWire = world.bus.lastPendingOfType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_NE(onWire, nullptr) << "sensor must have a sealed ADAPTER_DATA uplink frame in flight";
  // Correct header fields (origin/target MACs, proto_version, epoch, seq) —
  // exactly what an attacker could observe/replay off the air.
  mesh_message legit = *onWire;

  runPolled(2000);
  size_t before = hub->adapterDataFromOrigin(sensor->mac()).size();
  ASSERT_GT(before, 0u) << "the legit frame must be delivered first (baseline for the check below)";

  // Forge: same origin/target/epoch, a FRESH seq (so ReplayCache dedup can't
  // be the reason it's dropped), garbage auth tag the attacker never derived.
  mesh_message forged = legit;
  forged.seq_num = static_cast<uint16_t>(legit.seq_num + 1000);
  for (auto& b : forged.auth_tag)
    b = 0xAA;

  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), reinterpret_cast<const uint8_t*>(&forged), sizeof(forged));
  sim::swapOut(master->ctx());
  runPolled(1000);

  EXPECT_EQ(hub->adapterDataFromOrigin(sensor->mac()).size(), before)
      << "a forged frame with an invalid auth tag must be dropped, not delivered to the hub";
}

// Final-review fix: a broadcast-target (FF:FF:FF:FF:FF:FF) ADAPTER_DATA frame
// delivered at the master over the air must NOT reach the hub. No leaf ever
// originates a broadcast-target ADAPTER_DATA uplink — only the master's own
// downlink broadcast (Mesh::broadcastAdapterData, which delivers locally
// directly and never round-trips through processAdapterData) and beacons use
// FF:FF. Because addressedToSelf is false for a broadcast target, the old code
// skipped E2E open (needsOpen required addressedToSelf) and delivered the raw,
// unauthenticated frame straight to the serial/hub path — an RF attacker could
// forge arbitrary "sensor data" to the hub without ever enrolling or knowing
// any key material. Mirrors ForgedFrameWithoutValidTagIsDropped above, but
// targets FF:FF instead of the master's own MAC, uses a fresh seq, and plain
// (unsealed) attacker-controlled bytes with no valid auth tag at all.
TEST_F(MeshSimTest, ForgedBroadcastTargetFrameNeverReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->simulatePirMotion();
  world.step(1);
  mesh_message* onWire = world.bus.lastPendingOfType(MESH_TYPE_ADAPTER_DATA);
  ASSERT_NE(onWire, nullptr) << "sensor must have a sealed ADAPTER_DATA uplink frame in flight";
  // Correct/plausible header fields (origin MAC, proto_version, epoch) — exactly
  // what an attacker could observe/replay off the air.
  mesh_message legit = *onWire;

  runPolled(2000);
  size_t before = hub->received.size();
  ASSERT_GT(hub->adapterDataFromOrigin(sensor->mac()).size(), 0u)
      << "the legit frame must be delivered first (baseline for the check below)";

  // Forge: same origin/epoch, broadcast target instead of the master's MAC, a
  // FRESH seq (so ReplayCache dedup can't be the reason it's dropped),
  // arbitrary attacker-chosen plaintext, and no valid auth tag whatsoever.
  mesh_message forged = legit;
  memset(forged.target_mac_address, 0xFF, 6);
  forged.seq_num = static_cast<uint16_t>(legit.seq_num + 2000);
  for (auto& b : forged.data)
    b = 0x41; // arbitrary forged "sensor data"
  for (auto& b : forged.auth_tag)
    b = 0xAA;

  sim::swapIn(master->ctx());
  simulateReceive(sensor->mac(), reinterpret_cast<const uint8_t*>(&forged), sizeof(forged));
  sim::swapOut(master->ctx());
  runPolled(1000);

  EXPECT_EQ(hub->received.size(), before)
      << "a broadcast-target frame arriving at the master over the air must be dropped "
         "entirely — the hub must receive nothing new from it";
}
