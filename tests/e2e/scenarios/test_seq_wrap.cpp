// Task 7 (spec §2): a u16 seq_num wrap within a single boot must not reuse an
// AEAD nonce (nonce = epoch_num || seq_num || origin_mac). buildMessage()
// bumps and persists the boot epoch the instant nextSeq() wraps 0xFFFF -> 0,
// so seq_num == 0 never actually goes on the air and every (epoch, seq) pair
// stays unique for the life of the device.
//
// This scenario forces the wrap by hand (setting the leaf's in-memory
// txSeqNum to 0xFFFE via the UNIT_TEST-only testReplay() hook — no real
// device would send 65 thousand frames in a test's lifetime) and then drives
// three real uplinks through the actual buildMessage()/seal/send path via
// simulatePirMotion(). PirAdapter enforces a 3s post-trigger cooldown before
// it re-arms (see PirAdapter.cpp), so each trigger is followed by a
// runPolled() window comfortably longer than that before the next.
#include "harness/MeshSimTest.h"
#include "src/mesh/Mesh.h"
#include "src/persistence/EepromManager.h"

TEST_F(MeshSimTest, SeqWrapBumpsEpochAndKeepsSealing) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  // Force the leaf to the brink of a seq wrap.
  sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    mesh.testReplay().txSeqNum = 0xFFFE;
    return 0;
  });
  uint32_t epochBefore = sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    return mesh.testReplay().bootEpoch;
  });

  // Three uplinks: #1 -> seq 0xFFFF (no wrap yet). #2 -> nextSeq() wraps to 0,
  // buildMessage bumps+persists the epoch and re-draws a fresh seq (1) before
  // the frame is sealed/sent, so seq_num==0 never reaches the wire. #3 ->
  // seq 2, same (bumped) epoch. Net effect across all three: exactly one bump.
  sensor->simulatePirMotion();
  runPolled(4000); // > 3s PIR cooldown: re-arms before the next trigger
  sensor->simulatePirMotion();
  runPolled(4000);
  sensor->simulatePirMotion();
  runPolled(4000);

  uint32_t epochAfter = sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    return mesh.testReplay().bootEpoch;
  });
  EXPECT_EQ(epochAfter, epochBefore + 1)
      << "the in-memory boot epoch must bump exactly once across the wrap";

  // Post-wrap frames (#2, #3) still opened cleanly at the master — see below —
  // which only happens if the master's ReplayCache/AEAD-open path trusts each
  // frame's OWN epoch_num field rather than some stale/cached value, proving
  // the leaf actually stamped the bumped epoch onto the frame before sealing.
  auto frames = hub->adapterDataFromOrigin(sensor->mac());
  ASSERT_GE(frames.size(), 3u) << "all three uplinks must reach the hub decrypted, "
                                   "including the two sent after the epoch bump";
  for (size_t i = frames.size() - 3; i < frames.size(); ++i) {
    EXPECT_EQ(frames[i].data[0], 1)
        << "frame " << i
        << " must carry the original plaintext motion opcode — the master must have "
           "opened it successfully using the frame's own (possibly bumped) epoch_num";
  }

  // The bumped epoch must be durable, not just an in-RAM field: EepromManager
  // is a per-node singleton whose byte image lives in this leaf's NodeContext,
  // so with() (which swaps that context in/out around fn) is what makes this
  // read reflect the leaf's own persisted BOOT_EPOCH rather than whichever
  // node's state happened to be live globally beforehand.
  uint32_t persistedEpoch = sensor->with([](lattice::mesh::Mesh&, lattice::adapter::Adapter*) {
    return lattice::utils::EepromManager::getInstance().loadBootEpoch();
  });
  EXPECT_EQ(persistedEpoch, epochBefore + 1)
      << "the bumped epoch must be persisted to EEPROM, not just held in RAM — "
         "otherwise a reboot right after the wrap would replay the old epoch's seq range";
}

// Regression for the finding that the seq-wrap guard originally lived ONLY in
// Mesh::buildMessage — sendEnrollmentRequest() and enrollPeer() (JOIN_ACK) draw
// from the very same ReplayCache::txSeqNum via a raw replay.nextSeq() call, so
// a wrap landing on either of those un-sealed paths would silently resume
// seq 1,2,3... under the OLD (un-bumped) epoch; any later sealed buildMessage
// frame in the same boot would then reuse an AEAD nonce already used by
// pre-wrap traffic. Both call sites now route through the same
// Mesh::nextSeqGuarded() choke point as buildMessage(); this proves the
// enrollment-request path specifically.
//
// NOTE: the master's serial relay of an ENROLLMENT frame to the hub
// (SerialAdapter::relayEnrollmentToServer) rebuilds a brand-new message
// carrying only (mac, pubKey) — it does not forward the leaf's original
// seq_num/epoch_num — so hub->enrollmentFrom() can't observe the value this
// test cares about. Instead, inspect the raw over-the-air broadcast directly
// via VirtualBus::lastPendingOfType(), the same technique test_replay_e2e.cpp
// uses to capture an in-flight sealed frame before it's delivered.
TEST_F(MeshSimTest, EnrollmentRequestSeqWrapBumpsEpoch) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);

  // Force the very next draw from the leaf's txSeqNum to wrap: unlike the
  // buildMessage scenario above (which needs 0xFFFE -> two draws), a single
  // nextSeq() call against 0xFFFF wraps immediately, so the node's first
  // periodic ENROLLMENT broadcast (not yet approved -- still unenrolled) is
  // the one that must observe and apply the guard.
  sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    mesh.testReplay().txSeqNum = 0xFFFF;
    return 0;
  });
  uint32_t epochBefore = sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    return mesh.testReplay().bootEpoch;
  });

  ASSERT_FALSE(sensor->isEnrolled());

  // Single-ms steps (mirrors main.ino's `millis() - lastEnrollmentBroadcast >
  // 10000` gate) so we can catch the ENROLLMENT broadcast in VirtualBus's
  // one-step in-flight window and read its seq_num before the next step
  // delivers and clears it.
  uint16_t sentSeq = 0;
  bool sawRequest = false;
  for (uint32_t t = 0; t < 10500 && !sawRequest; ++t) {
    world.step(1);
    if (const mesh_message* pending = world.bus.lastPendingOfType(MESH_TYPE_ENROLLMENT)) {
      sentSeq = pending->seq_num;
      sawRequest = true;
    }
  }
  ASSERT_TRUE(sawRequest) << "enrollment request never sent within the retry window";
  EXPECT_NE(sentSeq, 0)
      << "the wrapped seq_num (0) must never reach the wire — sendEnrollmentRequest() "
         "must draw through the same guarded choke point as buildMessage()";

  uint32_t epochAfter = sensor->with([](lattice::mesh::Mesh& mesh, lattice::adapter::Adapter*) {
    return mesh.testReplay().bootEpoch;
  });
  EXPECT_EQ(epochAfter, epochBefore + 1)
      << "the boot epoch must bump exactly once when the enrollment-request path "
         "hits the seq wrap, same as the sealed buildMessage() path";
}
