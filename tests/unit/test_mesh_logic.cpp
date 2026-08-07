#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "error/Error.h"
#include "mesh/Mesh.h"
#include "mesh/MeshCrypto.h"
#include "mesh/E2ECrypto.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "lib/lattice-protocol/c/opcodes.h"
#include "config/master_pubkey_pin_wrapper.h"

using namespace lattice::mesh;

class MeshLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    // Phase D (#42): pin-active by default so tests exercise production
    // behaviour; individual tests below opt into the runtime bypass where
    // the scenario under test (stale-master hotswap, dual-master TOFU) uses
    // a MAC that legitimately differs from lattice::mesh::pin::MASTER_MAC.
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }

  mesh_message makeBeacon(const uint8_t masterMac[6], uint32_t epoch, uint16_t seq) {
    mesh_message m{};
    m.proto_version = 1;
    m.message_type = MESH_TYPE_MASTER_BEACON;
    m.epoch_num = epoch;
    m.seq_num = seq;
    memcpy(m.origin_mac_address, masterMac, 6);
    return m;
  }
};

// --- TOFU master MAC ---

TEST_F(MeshLogicTest, TOFU_FirstBeacon_LearnsMasterMAC) {
  Mesh mesh;
  // Simulate non-master node (hasMasterMac = false initially)
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  auto beacon = makeBeacon(masterMac, 1, 1);

  mesh.beacon.process(beacon, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  EXPECT_TRUE(mesh.enrollment.hasMasterMac);
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, masterMac, 6), 0);
}

TEST_F(MeshLogicTest, TOFU_SecondBeaconFromSameMAC_Accepted) {
  Mesh mesh;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  mesh.beacon.process(makeBeacon(masterMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Second beacon from same MAC — should update lastMasterSeenMs, not reject
  advanceMillis(3000);
  auto beacon2 = makeBeacon(masterMac, 1, 2);
  // No assertion — just verify no crash and relay fires
  mesh.beacon.process(beacon2, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_TRUE(mesh.enrollment.hasMasterMac);
}

TEST_F(MeshLogicTest, TOFU_BeaconFromImpostorMAC_Rejected_WhenMasterAlive) {
  Mesh mesh;
  const uint8_t realMaster[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t impostorMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

  // Learn real master
  mesh.beacon.process(makeBeacon(realMaster, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Impostor beacon arrives while real master is still fresh
  size_t sendsBefore = espNowSentPackets.size();
  mesh.beacon.process(makeBeacon(impostorMac, 1, 2), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Impostor should NOT be accepted as master
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, realMaster, 6), 0);
  // Relay should NOT fire for impostor
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}

TEST_F(MeshLogicTest, TOFU_NewMasterAccepted_AfterStaleTimeout) {
  // Phase D (#42): stale-hotswap-to-a-genuinely-different-MAC is pre-pin TOFU
  // behaviour — production now pins the primary MAC, so a real deployment
  // can't hotswap to different hardware without re-provisioning. This test
  // exercises the underlying hotswap logic in isolation (still reachable in
  // DEV_MODE / via the runtime bypass), hence the explicit bypass.
  lattice::mesh::pin::setTestBypass(true);
  Mesh mesh;
  const uint8_t oldMaster[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t newMaster[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.beacon.process(makeBeacon(oldMaster, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Advance past STALE_MASTER_THRESHOLD_MS (9000ms)
  advanceMillis(9001);

  mesh.beacon.process(makeBeacon(newMaster, 2, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, newMaster, 6), 0);
}

// --- Beacon relay dedup ---

TEST_F(MeshLogicTest, BeaconRelay_SameEpochSeq_SuppressedRelay) {
  Mesh mesh;
  // Set as non-master
  mesh.isMaster = false;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

  // First beacon — sets relay pending
  mesh.beacon.process(makeBeacon(masterMac, 1, 5), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_TRUE(mesh.relayPending);

  // Drain relay (simulate loop)
  mesh.relayPending = false;

  size_t sendsBefore = espNowSentPackets.size();
  // Same beacon arrives again (duplicate path, e.g. multi-hop echo)
  mesh.beacon.process(makeBeacon(masterMac, 1, 5), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  // Relay should NOT fire — same epoch+seq
  EXPECT_FALSE(mesh.relayPending);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}

TEST_F(MeshLogicTest, BeaconRelay_NewerSeq_AllowsRelay) {
  Mesh mesh;
  mesh.isMaster = false;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

  mesh.beacon.process(makeBeacon(masterMac, 1, 5), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  mesh.relayPending = false; // Drain

  mesh.beacon.process(makeBeacon(masterMac, 1, 6), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac); // Newer seq
  EXPECT_TRUE(mesh.relayPending);
}

// --- Dual master mode ---

TEST_F(MeshLogicTest, DualMaster_SecondBeaconFromNewMAC_LearnedAsSecondary) {
  // Phase D (#42): only the primary MAC is pinned (design §5) — a secondary
  // master's beacon fails the pin unconditionally in production, so
  // beacon-TOFU-learn-secondary is now pre-pin-only behaviour (production
  // dual-master trust for the secondary comes from the primary's
  // pin-authenticated JOIN_ACK instead, per design). Bypass to keep
  // exercising this still-present logic in isolation.
  lattice::mesh::pin::setTestBypass(true);
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  // Learn primary via first beacon
  mesh.beacon.process(makeBeacon(primaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  ASSERT_TRUE(mesh.enrollment.hasMasterMac);

  // Second beacon from different MAC — must be learned as secondary
  mesh.beacon.process(makeBeacon(secondaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  EXPECT_TRUE(mesh.enrollment.hasMasterMacSecondary);
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMacSecondary, secondaryMac, 6), 0);
  // Primary must still be unchanged
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, primaryMac, 6), 0);
}

TEST_F(MeshLogicTest, DualMaster_BeaconFromPrimaryMAC_Accepted) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.beacon.process(makeBeacon(primaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);   // learn primary
  mesh.beacon.process(makeBeacon(secondaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac); // learn secondary

  // Beacon from primary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.beacon.process(makeBeacon(primaryMac, 2, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known primary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_BeaconFromSecondaryMAC_Accepted) {
  // Phase D (#42): the secondary's MAC differs from the pinned primary MAC,
  // so every beacon it sends now fails the beacon pin unconditionally in
  // production (design §5 — secondary trust comes from the pin-authenticated
  // JOIN_ACK relay path, not beacon TOFU). Bypass to keep exercising the
  // underlying known-secondary relay-acceptance logic in isolation.
  lattice::mesh::pin::setTestBypass(true);
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.beacon.process(makeBeacon(primaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);   // learn primary
  mesh.beacon.process(makeBeacon(secondaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac); // learn secondary

  // Beacon from secondary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.beacon.process(makeBeacon(secondaryMac, 2, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known secondary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_ImpostorMAC_Rejected_WhenBothMastersKnown) {
  // Phase D (#42): needs a beacon-TOFU-learned secondary as precondition
  // (pre-pin behaviour, see DualMaster_SecondBeaconFromNewMAC_LearnedAsSecondary
  // above) so the "impostor rejected while both masters known" app-layer
  // logic — as opposed to plain pin rejection — is what's actually exercised
  // for the impostor beacon itself.
  lattice::mesh::pin::setTestBypass(true);
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t impostorMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

  mesh.beacon.process(makeBeacon(primaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);   // learn primary
  mesh.beacon.process(makeBeacon(secondaryMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac); // learn secondary

  // Third distinct MAC while both masters fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.beacon.process(makeBeacon(impostorMac, 1, 2), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Neither primary nor secondary should have changed
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, primaryMac, 6), 0);
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMacSecondary, secondaryMac, 6), 0);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore) << "Impostor beacon must not trigger relay";
}

TEST_F(MeshLogicTest, SingleMaster_SecondBeaconFromNewMAC_Rejected_WhenMasterAlive) {
  Mesh mesh;
  // _dualMasterMode defaults to false — no need to set
  const uint8_t knownMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t unknownMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.beacon.process(makeBeacon(knownMac, 1, 1), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  // Second distinct MAC while single master still fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.beacon.process(makeBeacon(unknownMac, 1, 2), mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);

  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, knownMac, 6), 0)
      << "Known master MAC must not change";
  EXPECT_FALSE(mesh.enrollment.hasMasterMacSecondary);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}

// ─── MeshBeaconPinTest ───────────────────────────────────────────────────────
// Phase D (#42): beacon processing (MasterBeacon::process, Phase B Task 5 —
// was Mesh::processMasterBeacon) requires the beacon's origin_mac_address to
// match the compile-time-pinned
// lattice::mesh::pin::MASTER_MAC before any TOFU learn/accept logic runs.
// Weaker guarantee than the JOIN_ACK pubkey pin (WiFi MACs are spoofable),
// but rejects naive attackers before any state mutation.

class MeshBeaconPinTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }
};

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_ValidOriginMac_Learns) {
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  memcpy(b.last_hop_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  b.hop_count = 0;
  m.beacon.process(b, m.deviceMacAddress, m.isMaster, m._dualMasterMode, m.enrollment, m.neighbors, m.currentMaster, m.txState, m.relayPendingMsg, m.relayPendingAt, m.relayPending, m.lastSeenMasterMac);
  EXPECT_TRUE(m.enrollment.hasMasterMac);
}

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_WrongOriginMac_Drops) {
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.origin_mac_address[0] ^= 0xFF;
  memcpy(b.last_hop_mac_address, b.origin_mac_address, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  m.beacon.process(b, m.deviceMacAddress, m.isMaster, m._dualMasterMode, m.enrollment, m.neighbors, m.currentMaster, m.txState, m.relayPendingMsg, m.relayPendingAt, m.relayPending, m.lastSeenMasterMac);
  EXPECT_FALSE(m.enrollment.hasMasterMac);
}

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_TestBypass_SkipsCheck) {
  lattice::mesh::pin::setTestBypass(true);
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.origin_mac_address[0] ^= 0xFF;
  memcpy(b.last_hop_mac_address, b.origin_mac_address, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  m.beacon.process(b, m.deviceMacAddress, m.isMaster, m._dualMasterMode, m.enrollment, m.neighbors, m.currentMaster, m.txState, m.relayPendingMsg, m.relayPendingAt, m.relayPending, m.lastSeenMasterMac);
  EXPECT_TRUE(m.enrollment.hasMasterMac);
}

// ─── relayDownlink ───────────────────────────────────────────────────────────

class RelayDownlinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kPeer1Mac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x01};
  static constexpr uint8_t kPeer2Mac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0x02};
  static constexpr uint8_t kOriginMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x03};

  mesh_message makeDataMsg(const uint8_t origin[6], const uint8_t target[6], uint32_t epoch,
                           uint16_t seq, uint8_t hopCount = 0) {
    mesh_message m{};
    m.proto_version = 1;
    m.message_type = MESH_TYPE_ADAPTER_DATA;
    m.data_type = adapter_types::PIR_ADAPTER;
    memcpy(m.origin_mac_address, origin, 6);
    memcpy(m.target_mac_address, target, 6);
    memcpy(m.last_hop_mac_address, origin, 6);
    m.hop_count = hopCount;
    m.epoch_num = epoch;
    m.seq_num = seq;
    return m;
  }
};

constexpr uint8_t RelayDownlinkTest::kMyMac[];
constexpr uint8_t RelayDownlinkTest::kPeer1Mac[];
constexpr uint8_t RelayDownlinkTest::kPeer2Mac[];
constexpr uint8_t RelayDownlinkTest::kOriginMac[];

TEST_F(RelayDownlinkTest, SendsToPeers_IncrementHopCount) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMs = 0;
  mesh.peers.append(p1);
  PeerInfo p2{};
  memcpy(p2.mac, kPeer2Mac, 6);
  p2.lastSeenMs = 0;
  mesh.peers.append(p2);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1, /*hopCount=*/1);

  mesh.router.relayDownlink(msg, mesh.peers, mesh.deviceMacAddress, mesh.transport);

  // 2 peers → 2 sends
  EXPECT_EQ(espNowSentPackets.size(), 2u);
  for (const auto& pkt : espNowSentPackets) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(pkt.data.data());
    EXPECT_EQ(sent.hop_count, 2u);                               // incremented
    EXPECT_EQ(memcmp(sent.target_mac_address, kPeer2Mac, 6), 0); // target preserved
    EXPECT_EQ(memcmp(sent.last_hop_mac_address, kMyMac, 6), 0);  // lastHop = my MAC
  }
}

TEST_F(RelayDownlinkTest, DropsAtMaxHops) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMs = 0;
  mesh.peers.append(p1);

  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 1,
                         /*hopCount=*/lattice::config::MAX_HOPS);

  mesh.router.relayDownlink(msg, mesh.peers, mesh.deviceMacAddress, mesh.transport);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, SkipsSelf_WhenSelfInPeerList) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMs = 0;
  mesh.peers.append(p1);
  // Add self to peer list (shouldn't happen in production but guard against it)
  PeerInfo self{};
  memcpy(self.mac, kMyMac, 6);
  self.lastSeenMs = 0;
  mesh.peers.append(self);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1);
  mesh.router.relayDownlink(msg, mesh.peers, mesh.deviceMacAddress, mesh.transport);

  // Only 1 peer (kPeer1Mac) — self skipped
  EXPECT_EQ(espNowSentPackets.size(), 1u);
  EXPECT_EQ(memcmp(espNowSentPackets[0].addr, kPeer1Mac, 6), 0);
}

// ─── processAdapterData: uplink relay ────────────────────────────────────────

class AdapterDataRelayTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kSensorMac[6] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
  static constexpr uint8_t kPeerMac[6] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};

  Mesh makeIntermediateNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false;
    // Set master route: next hop IS the master (1 hop away)
    memcpy(mesh.currentMaster.mac, kMasterMac, 6);
    mesh.currentMaster.distance = 1;
    mesh.enrollment.hasMasterMac = true;
    memcpy(mesh.enrollment.knownMasterMac, kMasterMac, 6);
    // Register master as enrolled peer (required for sendMessage + isPeerInRange)
    PeerInfo p{};
    memcpy(p.mac, kMasterMac, 6);
    p.lastSeenMs = 0;
    mesh.peers.append(p);
    return mesh;
  }

  mesh_message makeUplinkMsg(uint32_t epoch, uint16_t seq, uint8_t hopCount = 1) {
    mesh_message m{};
    m.proto_version = 1;
    m.message_type = MESH_TYPE_ADAPTER_DATA;
    m.data_type = adapter_types::PIR_ADAPTER;
    memcpy(m.origin_mac_address, kSensorMac, 6);
    memcpy(m.target_mac_address, kMasterMac, 6); // addressed to master
    memcpy(m.last_hop_mac_address, kSensorMac, 6);
    m.hop_count = hopCount;
    m.epoch_num = epoch;
    m.seq_num = seq;
    return m;
  }
};

constexpr uint8_t AdapterDataRelayTest::kMyMac[];
constexpr uint8_t AdapterDataRelayTest::kMasterMac[];
constexpr uint8_t AdapterDataRelayTest::kSensorMac[];
constexpr uint8_t AdapterDataRelayTest::kPeerMac[];

TEST_F(AdapterDataRelayTest, IntermediateNode_RelaysUplinkTowardMaster) {
  Mesh mesh = makeIntermediateNode();
  auto msg = makeUplinkMsg(1, 1, /*hopCount=*/1);

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_EQ(espNowSentPackets.size(), before + 1);
  const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.hop_count, 2u);                                      // incremented
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kMasterMac, 6), 0); // routed via nextHop
}

TEST_F(AdapterDataRelayTest, Master_DoesNotRelayUplink_DeliversLocally) {
  Mesh mesh = makeIntermediateNode();
  mesh.isMaster = true;

  // Task 6 (E2E AEAD): the master now opens sealed ADAPTER_DATA before local
  // delivery, so the test frame must be genuinely sealed with keys the master
  // can actually derive (peerE2EKeys uses the master's own priv + the
  // registered origin peer's pubkey; ECDH is symmetric, so sealing with the
  // sensor's priv + the master's pubkey yields the same k_up).
  uint8_t masterPriv[32], masterPub[32], sensorPriv[32], sensorPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(sensorPriv, sensorPub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo sensorPeer{};
  memcpy(sensorPeer.mac, kSensorMac, 6);
  memcpy(sensorPeer.publicKey, sensorPub, 32);
  sensorPeer.lastSeenMs = 0;
  mesh.peers.append(sensorPeer);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(sensorPriv, masterPub, kUp, kDown);

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](const mesh_message&) { callbackFired = true; });

  auto msg = makeUplinkMsg(1, 1);
  memcpy(msg.target_mac_address, mesh.deviceMacAddress, 6); // addressed to self (master)
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg)); // seal after target is final

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_TRUE(callbackFired);
  EXPECT_EQ(espNowSentPackets.size(), before); // no relay
}

// ─── processAdapterData: downlink + broadcast relay ──────────────────────────

TEST_F(AdapterDataRelayTest, IntermediateNode_RelaysDownlinkToOtherTarget) {
  // Node receives ADAPTER_DATA addressed to a different sensor — must relay outward
  Mesh mesh = makeIntermediateNode();
  // Add a second peer (different from master) to relay toward
  PeerInfo extra{};
  memcpy(extra.mac, kPeerMac, 6);
  extra.lastSeenMs = 0;
  mesh.peers.append(extra);

  mesh_message msg{};
  msg.proto_version = 1;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kMasterMac, 6);
  memcpy(msg.target_mac_address, kSensorMac, 6); // some other sensor, not me, not master
  msg.hop_count = 1;
  msg.epoch_num = 2;
  msg.seq_num = 1;

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  // Should relay to all peers: kMasterMac + kPeerMac (2 peers)
  EXPECT_GT(espNowSentPackets.size(), before);
  // Target preserved in every relayed copy
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.target_mac_address, kSensorMac, 6), 0);
    EXPECT_EQ(sent.hop_count, 2u);
  }
}

TEST_F(AdapterDataRelayTest, IntermediateNode_BroadcastTarget_DeliveredAndRelayed) {
  Mesh mesh = makeIntermediateNode();
  PeerInfo extra{};
  memcpy(extra.mac, kPeerMac, 6);
  extra.lastSeenMs = 0;
  mesh.peers.append(extra);

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](const mesh_message&) { callbackFired = true; });

  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  mesh_message msg{};
  msg.proto_version = 1;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kMasterMac, 6);
  memcpy(msg.target_mac_address, kBroadcast, 6); // broadcast
  msg.hop_count = 1;
  msg.epoch_num = 3;
  msg.seq_num = 1;

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  EXPECT_TRUE(callbackFired);                  // delivered locally
  EXPECT_GT(espNowSentPackets.size(), before); // AND relayed outward
}

TEST_F(AdapterDataRelayTest, BroadcastAdapterData_UsesBroadcastTargetMAC) {
  // Verify master's broadcastAdapterData sets FF:FF target so multi-hop works
  Mesh mesh = makeIntermediateNode();
  mesh.isMaster = true;
  // Add a peer so broadcastToAllPeers has someone to send to
  PeerInfo extra{};
  memcpy(extra.mac, kPeerMac, 6);
  extra.lastSeenMs = 0;
  mesh.peers.append(extra);

  static constexpr uint8_t kPayload[64] = {0x01, 0x02, 0x03};
  size_t before = espNowSentPackets.size();
  mesh.broadcastAdapterData(adapter_types::PIR_ADAPTER, kPayload);

  EXPECT_GT(espNowSentPackets.size(), before);
  // Every sent message should have FF:FF target
  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.target_mac_address, kBroadcast, 6), 0);
  }
}

// Bug #5 regression: a non-master node that hears an ENROLLMENT broadcast from a
// node further from the master must relay it one hop toward the master, so a leaf
// out of direct RF range of the master can still enroll.
TEST_F(AdapterDataRelayTest, IntermediateNode_RelaysEnrollmentTowardMaster) {
  Mesh mesh = makeIntermediateNode();

  mesh_message req{};
  req.message_type = MESH_TYPE_ENROLLMENT;
  req.data_type = adapter_types::UNKNOWN_ADAPTER;
  memcpy(req.origin_mac_address, kSensorMac, 6); // originated by a distant leaf
  memset(req.target_mac_address, 0xFF, 6);       // enrollment is broadcast
  memcpy(req.last_hop_mac_address, kSensorMac, 6);
  req.hop_count = 0;

  size_t before = espNowSentPackets.size();
  mesh.relayEnrollmentUplink(req);

  ASSERT_EQ(espNowSentPackets.size(), before + 1) << "must relay one hop toward master";
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kMasterMac, 6), 0)
      << "relay must be routed to the next hop toward master";
  const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.message_type, MESH_TYPE_ENROLLMENT);
  EXPECT_EQ(sent.hop_count, 1u) << "hop_count incremented on relay";
  EXPECT_EQ(memcmp(sent.origin_mac_address, kSensorMac, 6), 0) << "origin preserved";
  EXPECT_EQ(memcmp(sent.last_hop_mac_address, kMyMac, 6), 0) << "last hop stamped as relay";
}

TEST_F(AdapterDataRelayTest, IntermediateNode_DoesNotRelayOwnEnrollment) {
  Mesh mesh = makeIntermediateNode();

  mesh_message req{};
  req.message_type = MESH_TYPE_ENROLLMENT;
  memcpy(req.origin_mac_address, kMyMac, 6); // our OWN outbound request echoed back
  memset(req.target_mac_address, 0xFF, 6);
  memcpy(req.last_hop_mac_address, kMyMac, 6);
  req.hop_count = 0;

  size_t before = espNowSentPackets.size();
  mesh.relayEnrollmentUplink(req);

  EXPECT_EQ(espNowSentPackets.size(), before) << "must not relay our own request";
}

// ─── processJoinAck: relay ───────────────────────────────────────────────────

class JoinAckRelayTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kDistantNode[6] = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44};
  static constexpr uint8_t kPeerMac[6] = {0x33, 0x33, 0x33, 0x33, 0x33, 0x33};

  Mesh makeIntermediateNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false; // explicit defensive guard
    PeerInfo p{};
    memcpy(p.mac, kPeerMac, 6);
    p.lastSeenMs = 0;
    mesh.peers.append(p);
    return mesh;
  }

  // A master with one enrolled leaf (real Curve25519 keys, so peerE2EKeys can
  // actually derive k_down for sendDownlinkToNode's sealing step).
  Mesh makeMasterNode() {
    Mesh mesh;
    static constexpr uint8_t kThisMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    static constexpr uint8_t kLeafMac[6] = {0x02, 0, 0, 0, 0, 0x0B};
    memcpy(mesh.deviceMacAddress, kThisMasterMac, 6);
    mesh.isMaster = true;
    uint8_t masterPriv[32], masterPub[32], leafPriv[32], leafPub[32];
    lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
    lattice::mesh::crypto::generateKeypair(leafPriv, leafPub);
    memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
    memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
    PeerInfo leaf{};
    memcpy(leaf.mac, kLeafMac, 6);
    memcpy(leaf.publicKey, leafPub, 32);
    leaf.lastSeenMs = 0;
    mesh.peers.append(leaf);
    // isMaster is set directly above (not via setIsMaster()+init()), so the
    // RouteTable allocation Mesh::init() would normally trigger never runs —
    // do it explicitly (issue #51).
    mesh.reevaluateRouteTable();
    return mesh;
  }

  // A bare relay node identified only by its MAC — used to exercise the
  // stateless downlink forwarding branch of processAdapterData in isolation.
  Mesh makeIntermediateNodeWithMac(const uint8_t mac[6]) {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, mac, 6);
    mesh.isMaster = false;
    return mesh;
  }

  mesh_message makeJoinAck(const uint8_t target[6], uint8_t hopCount = 1) {
    mesh_message m{};
    m.proto_version = 1;
    m.message_type = MESH_TYPE_JOIN_ACK;
    m.data_type = adapter_types::UNKNOWN_ADAPTER;
    memcpy(m.origin_mac_address, kMasterMac, 6);
    memcpy(m.target_mac_address, target, 6);
    memcpy(m.last_hop_mac_address, kMasterMac, 6);
    m.hop_count = hopCount;
    m.epoch_num = 1;
    m.seq_num = 1;
    return m;
  }
};

constexpr uint8_t JoinAckRelayTest::kMyMac[];
constexpr uint8_t JoinAckRelayTest::kMasterMac[];
constexpr uint8_t JoinAckRelayTest::kDistantNode[];
constexpr uint8_t JoinAckRelayTest::kPeerMac[];

TEST_F(JoinAckRelayTest, RelaysJoinAck_WhenNotAddressedToSelf) {
  Mesh mesh = makeIntermediateNode();

  auto msg = makeJoinAck(kDistantNode); // addressed to a distant node, not me

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  // Task 9b: re-broadcast (not unicast-to-peers) so the still-unenrolled distant
  // node — which is not yet a registered unicast peer — can hear the ACK.
  ASSERT_EQ(espNowSentPackets.size(), before + 1);
  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kBroadcast, 6), 0)
      << "JOIN_ACK relay must be broadcast";
  const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.hop_count, 2u);
  EXPECT_EQ(memcmp(sent.target_mac_address, kDistantNode, 6), 0); // target preserved
  EXPECT_EQ(memcmp(sent.last_hop_mac_address, kMyMac, 6), 0);     // last hop stamped as us
}

TEST_F(JoinAckRelayTest, DoesNotRelayJoinAck_WeOriginated) {
  // Loop safety: a node (a master) must never re-broadcast a JOIN_ACK it emitted.
  Mesh mesh = makeIntermediateNode();

  auto msg = makeJoinAck(kDistantNode);
  memcpy(msg.origin_mac_address, kMyMac, 6); // we originated it

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  EXPECT_EQ(espNowSentPackets.size(), before) << "must not relay our own JOIN_ACK";
}

TEST_F(JoinAckRelayTest, DoesNotRelayJoinAck_WhenAddressedToSelf) {
  // When addressed to self: process (enroll), do NOT relay
  Mesh mesh = makeIntermediateNode();

  // Provide a fingerprint (first 4 bytes of devicePublicKey)
  // enrollment.devicePublicKey is zeroed in constructor — fingerprint = {0,0,0,0}
  auto msg = makeJoinAck(kMyMac);
  memset(msg.data, 0, sizeof(msg.data)); // fingerprint matches zeroed pubkey

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no relay
}

TEST_F(JoinAckRelayTest, JoinAckAddressedToSelf_RegistersMasterAsRoutablePeer) {
  // An accepted JOIN_ACK must add the approving master (origin MAC + the
  // master public key carried in enrollment_public_key) to the node's own
  // PeerRegistry — findNextHopToMaster() can only route through registry
  // entries, so without this the enrolled node has no uplink route at all.
  Mesh mesh = makeIntermediateNode();
  ASSERT_EQ(mesh.peers.find(kMasterMac), nullptr) << "precondition: master not yet a peer";

  // Real Curve25519 keypair for the node: ESP-NOW peer registration no longer
  // derives a per-peer LMK (Task 8 — link layer is unencrypted; E2E AEAD is
  // the security boundary), but the stored public key still feeds E2E key
  // derivation elsewhere, so keep using a real (non-zeroed) key here.
  // The master's key must equal the Phase D (#42) pin — masterKey is what the
  // JOIN_ACK carries as enrollment_public_key, and Enrollment::processJoinAck
  // now rejects anything that doesn't match lattice::mesh::pin::MASTER_PUBKEY.
  // This test is about peer-registration mechanics, not pin verification, so
  // seed the "master" key from the pin rather than a random keypair.
  uint8_t nodePriv[32], nodePub[32];
  uint8_t masterKey[32];
  lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
  memcpy(masterKey, lattice::mesh::pin::MASTER_PUBKEY, 32);
  memcpy(mesh.enrollment.devicePrivateKey, nodePriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, nodePub, 32);

  auto msg = makeJoinAck(kMyMac);
  memcpy(msg.data, nodePub, 4); // fingerprint = first 4 bytes of node pubkey
  memcpy(msg.enrollment_public_key, masterKey, 32);

  mesh.processJoinAck(msg);

  PeerInfo* master = mesh.peers.find(kMasterMac);
  ASSERT_NE(master, nullptr) << "master must be registered in the node's PeerRegistry";
  EXPECT_EQ(memcmp(master->publicKey, masterKey, 32), 0)
      << "master's public key from the JOIN_ACK must be stored";

  // And the route must actually resolve once a beacon establishes the topology
  // (nextHop = master, one hop) — the end goal of registering the master.
  memcpy(mesh.currentMaster.mac, kMasterMac, 6);
  mesh.currentMaster.distance = 1;
  EXPECT_NE(mesh.findNextHopToMaster(), nullptr)
      << "uplink route must resolve through the newly registered master peer";
}

TEST_F(JoinAckRelayTest, ProcessJoinAckRegistersSecondaryMasterAndKeys) {
  // A JOIN_ACK carrying a non-zero secondary master identity (spec §5 dual
  // master) must register the secondary as a routable/keyable PeerRegistry
  // peer (mac+pubkey, persisted) AND record it as the TOFU secondary — this
  // is what lets masterE2EKeys() derive keys against the secondary once
  // currentMaster.mac flips to it after failover.
  Mesh leaf = makeIntermediateNode(); // non-master, not yet enrolled with anyone

  // Primary's key must equal the Phase D (#42) pin: Enrollment::processJoinAck
  // now rejects any enrollment_public_key that doesn't match
  // lattice::mesh::pin::MASTER_PUBKEY, and this test is about secondary-master
  // registration mechanics, not pin verification — the secondary's key is
  // NOT pin-checked (spec §5: only the primary's identity is pinned), so it
  // stays a real generated keypair.
  uint8_t leafPriv[32], leafPub[32], secPriv[32], secPub[32];
  uint8_t primPub[32];
  lattice::mesh::crypto::generateKeypair(leafPriv, leafPub);
  memcpy(primPub, lattice::mesh::pin::MASTER_PUBKEY, 32);
  lattice::mesh::crypto::generateKeypair(secPriv, secPub);
  memcpy(leaf.enrollment.devicePrivateKey, leafPriv, 32);
  memcpy(leaf.enrollment.devicePublicKey, leafPub, 32);

  const uint8_t primMac[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t secMac[6] = {0x02, 0, 0, 0, 0, 0x02};
  mesh_message ack = {};
  ack.proto_version = PROTO_VERSION;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.origin_mac_address, primMac, 6); // primary sent it
  memcpy(ack.target_mac_address, leaf.testDeviceMac(), 6);
  memcpy(ack.data, leafPub, 4); // fingerprint of the leaf's pubkey
  memcpy(ack.enrollment_public_key, primPub, 32); // primary pubkey
  // Protocol v0.6.0 (wire shrink §8): secondary master identity packed into
  // data[4..42] rather than top-level fields.
  memcpy(ack.data + 4, secMac, 6);
  memcpy(ack.data + 10, secPub, 32);

  leaf.processJoinAck(ack);

  // Secondary registered as a routable/keyable peer with its pubkey:
  PeerInfo* sec = leaf.peers.find(secMac);
  ASSERT_NE(sec, nullptr) << "secondary master must be registered in the PeerRegistry";
  EXPECT_EQ(0, memcmp(sec->publicKey, secPub, 32));
  // Secondary TOFU state set:
  EXPECT_TRUE(leaf.enrollment.hasMasterMacSecondary);
  EXPECT_EQ(0, memcmp(leaf.enrollment.knownMasterMacSecondary, secMac, 6));
  // And post-failover masterE2EKeys resolves against the secondary:
  leaf.enrollment.hasMasterMac = true; // enrolled with primary
  memcpy(leaf.currentMaster.mac, secMac, 6); // simulate adoption after failover
  leaf.currentMaster.distance = 1;
  const uint8_t *kUp, *kDown;
  EXPECT_TRUE(leaf.masterE2EKeys(&kUp, &kDown))
      << "keys derivable against the secondary post-failover";
}

TEST_F(JoinAckRelayTest, NextHopThroughRelayIsRegisteredAsEspNowPeer) {
  Mesh mesh = makeIntermediateNode(); // distance/enrollment set up by fixture
  // Node is distance 2; a relay at distance 1 is known ONLY via the NeighborTable
  // (never enrolled → never in PeerRegistry).
  const uint8_t relayMac[6] = {0x02, 0, 0, 0, 0, 0x77};
  mesh.currentMaster.distance = 2;
  mesh.testNeighbors().observe(relayMac, 1, mesh.testMillisNow());

  resetEspNowMock(); // clear recorded peers (mirror the mock's reset used elsewhere)
  PeerInfo* hop = mesh.findNextHopToMaster();

  ASSERT_NE(hop, nullptr) << "distance-2 node must route through the distance-1 relay";
  EXPECT_EQ(memcmp(hop->mac, relayMac, 6), 0);
  EXPECT_TRUE(esp_now_is_peer_exist(relayMac))
      << "relay must be auto-registered as an ESP-NOW peer";
}

// A distance-1 node whose enrolled master peer has gone stale (out of range)
// must not blackhole its uplink — the NeighborTable fallback branch of
// findNextHopToMaster() must still find a route if the master is also known
// as a fresh distance-0 neighbor (e.g. its own beacon was still heard even
// though the direct PeerRegistry entry's lastSeenMs is stale).
TEST_F(JoinAckRelayTest, DirectMasterStaleFallsBackToNeighborTable) {
  Mesh node = makeIntermediateNode();
  const uint8_t masterMac[6] = {0x02, 0, 0, 0, 0, 0x01};
  node.currentMaster.distance = 1;
  memcpy(node.currentMaster.mac, masterMac, 6);

  // Master is an enrolled peer but STALE (out of range): register it, then
  // advance the mocked clock past STALE_PEER_THRESHOLD_MS so
  // PeerRegistry::isPeerInRange() returns false for it.
  PeerInfo masterPeer{};
  memcpy(masterPeer.mac, masterMac, 6);
  masterPeer.lastSeenMs = 0;
  node.peers.append(masterPeer);
  advanceMillis(lattice::config::STALE_PEER_THRESHOLD_MS);

  // Master is also a fresh distance-0 neighbor:
  node.testNeighbors().observe(masterMac, 0, node.testMillisNow());

  PeerInfo* hop = node.findNextHopToMaster();
  ASSERT_NE(hop, nullptr) << "stale direct peer must fall back to the fresh NeighborTable entry";
  EXPECT_EQ(0, memcmp(hop->mac, masterMac, 6));
  // Branch-identity proof: only the NeighborTable fallback branch auto-registers
  // the next hop as an ESP-NOW peer; the direct-peer branch does not. So a
  // registered peer here proves the fallback fired, not the (stale) direct branch.
  EXPECT_TRUE(esp_now_is_peer_exist(masterMac));
}

// Final-review fix: findNextHopToMaster() must bound auto-registered
// forwarding ESP-NOW peers to exactly one, evicting the stale relay when the
// selected next hop changes — otherwise an RF attacker flooding distinct-MAC
// spoofed distance-1 beacons exhausts the ~20-slot ESP-NOW peer table
// permanently (no self-heal, no reboot), blackholing the real uplink.
TEST_F(JoinAckRelayTest, MultiHopForwardingPeer_BoundToOne_EvictsStaleRelayOnSwitch) {
  Mesh mesh = makeIntermediateNode(); // kPeerMac is an ENROLLED peer — must never be evicted
  memcpy(mesh.currentMaster.mac, kMasterMac, 6);
  mesh.currentMaster.distance = 3; // multi-hop: NeighborTable path, not the direct-peer branch
  // Mirror what setupEspNow()/addPeer() do for a real enrolled peer at boot:
  // register it as an ESP-NOW peer. The fixture's plain peers.append() above
  // only populates the PeerRegistry, not the ESP-NOW peer table.
  MeshTransport::registerPeerWithEspNow(kPeerMac);

  const uint8_t r1Mac[6] = {0x01, 0, 0, 0, 0, 0x01};
  const uint8_t r2Mac[6] = {0x02, 0, 0, 0, 0, 0x02};

  // R1 observed first, distance 1 from master.
  mesh.testNeighbors().observe(r1Mac, 1, mesh.testMillisNow());
  PeerInfo* hop1 = mesh.findNextHopToMaster();
  ASSERT_NE(hop1, nullptr);
  EXPECT_EQ(memcmp(hop1->mac, r1Mac, 6), 0);
  EXPECT_TRUE(esp_now_is_peer_exist(r1Mac)) << "R1 must be auto-registered on first forward";
  EXPECT_TRUE(esp_now_is_peer_exist(kPeerMac)) << "enrolled peer must remain registered";

  // R2 observed later (fresher), also distance 1 — freshest wins, selectNextHop
  // now returns R2 instead of R1.
  advanceMillis(1000);
  mesh.testNeighbors().observe(r2Mac, 1, mesh.testMillisNow());

  PeerInfo* hop2 = mesh.findNextHopToMaster();
  ASSERT_NE(hop2, nullptr);
  EXPECT_EQ(memcmp(hop2->mac, r2Mac, 6), 0) << "freshest relay (R2) must now be selected";
  EXPECT_TRUE(esp_now_is_peer_exist(r2Mac)) << "R2 must be auto-registered as the new next hop";
  EXPECT_FALSE(esp_now_is_peer_exist(r1Mac))
      << "stale forwarding peer R1 must be de-registered on switch";
  EXPECT_TRUE(esp_now_is_peer_exist(kPeerMac))
      << "enrolled peer must never be evicted by forwarding-peer churn";

  // Bound: enrolled peer (kPeerMac) + at most one auto-registered forwarding peer.
  EXPECT_EQ(espNowRegisteredPeers.size(), 2u)
      << "auto-registered forwarding peers must stay bounded to one";
}

// ─── sendDownlinkToNode / source-routed downlink relay ──────────────────────
// Helpers to inspect captured ESP-NOW sends by destination MAC — esp_now_send
// serializes to raw bytes, so deserialize back into a mesh_message to inspect.

static bool wasSentTo(const uint8_t* mac) {
  for (const auto& pkt : espNowSentPackets) {
    if (!pkt.isBroadcast && memcmp(pkt.addr, mac, 6) == 0)
      return true;
  }
  return false;
}

static mesh_message lastEspNowSentTo(const uint8_t* mac) {
  for (auto it = espNowSentPackets.rbegin(); it != espNowSentPackets.rend(); ++it) {
    if (!it->isBroadcast && memcmp(it->addr, mac, 6) == 0) {
      mesh_message m{};
      memcpy(&m, it->data.data(), sizeof(m));
      return m;
    }
  }
  ADD_FAILURE() << "no ESP-NOW packet was sent to the requested MAC";
  return mesh_message{};
}

// (a) Master with a known route source-routes to the first hop, target=dest, sealed.
TEST_F(JoinAckRelayTest, DownlinkSourceRoutesViaFirstHop) {
  Mesh master = makeMasterNode(); // fixture master with an enrolled leaf + keys
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
  const uint8_t R2[6] = {0x02, 0, 0, 0, 0, 0x22};
  uint8_t path[12];
  memcpy(path, R1, 6);
  memcpy(path + 6, R2, 6); // origin->R1->R2->master
  ASSERT_NE(master.testRoutes(), nullptr) << "master fixture must allocate RouteTable";
  master.testRoutes()->record(leaf, path, 2, 1000);

  resetEspNowMock();
  uint8_t cmd[64] = {};
  cmd[0] = OP_CONFIG_SET;
  master.sendDownlinkToNode(leaf, adapter_types::SERIAL_ADAPTER, cmd);

  // Reversed path [R2,R1]; first hop = R2.
  ASSERT_TRUE(wasSentTo(R2));
  mesh_message sent = lastEspNowSentTo(R2);
  EXPECT_EQ(sent.route_len, 2);
  EXPECT_EQ(0, memcmp(&sent.route_path[0], R2, 6));
  EXPECT_EQ(0, memcmp(&sent.route_path[6], R1, 6));
  EXPECT_EQ(0, memcmp(sent.target_mac_address, leaf, 6));
  // payload sealed: data[0] != plaintext opcode
  EXPECT_NE(sent.data[0], OP_CONFIG_SET);
  // first hop auto-registered as ESP-NOW peer (VirtualBus doesn't enforce this,
  // so assert it explicitly — the Phase-2 lesson).
  EXPECT_TRUE(esp_now_is_peer_exist(R2));
}

// Task 4 (Phase 5 follow-ups): a relayed ADAPTER_DATA frame is already sealed
// against the ORIGIN's target (AAD-bound — see E2ECrypto buildAad). A relay
// mid-forward must not overwrite target_mac_address with its OWN currentMaster
// — in a multi-master mesh where the relay and the frame's origin have
// adopted different masters (e.g. mid-failover), that rewrite corrupts the
// AAD the origin master needs to openPayload.
TEST_F(JoinAckRelayTest, RelayedAdapterDataKeepsOriginTarget) {
  Mesh relay = makeIntermediateNode(); // non-master, kPeerMac enrolled (unused here)
  const uint8_t originMaster[6] = {0x02, 0, 0, 0, 0, 0x01}; // the origin's master (frame target)
  // Force the relay's own currentMaster to a DIFFERENT mac (simulate multi-master/mid-failover):
  const uint8_t relaysMaster[6] = {0x02, 0, 0, 0, 0, 0x02};
  // Seed a peer entry for the relay's own master so findNextHopToMaster()
  // resolves via the direct-peer branch (distance 1, in range) and the frame
  // is actually sent.
  PeerInfo relayMasterPeer{};
  memcpy(relayMasterPeer.mac, relaysMaster, 6);
  relayMasterPeer.lastSeenMs = 0;
  relay.peers.append(relayMasterPeer);
  memcpy(relay.currentMaster.mac, relaysMaster, 6);
  relay.currentMaster.distance = 1;

  mesh_message fwd = {};
  fwd.proto_version = PROTO_VERSION;
  fwd.message_type = MESH_TYPE_ADAPTER_DATA;
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  memcpy(fwd.origin_mac_address, leaf, 6);         // foreign origin (a leaf)
  memcpy(fwd.target_mac_address, originMaster, 6); // sealed against originMaster's AAD
  fwd.epoch_num = 3;
  fwd.seq_num = 5;

  resetEspNowMock();
  relay.transmitCore(static_cast<adapter_types>(fwd.data_type), fwd.data, MESH_TYPE_ADAPTER_DATA,
                     &fwd);

  ASSERT_TRUE(wasSentTo(relaysMaster)) << "relay must forward toward its own next hop";
  mesh_message sent = lastEspNowSentTo(relaysMaster);
  EXPECT_EQ(0, memcmp(sent.target_mac_address, originMaster, 6))
      << "relayed frame must keep the origin's target (AAD-bound), not the relay's currentMaster";
}

// (b) A relay in the path forwards to the next index, unchanged frame.
TEST_F(JoinAckRelayTest, DownlinkRelayForwardsToNextIndex) {
  static constexpr uint8_t kR2[6] = {0x02, 0, 0, 0, 0, 0x22};
  Mesh r2 = makeIntermediateNodeWithMac(kR2);
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
  mesh_message dl = {};
  dl.proto_version = PROTO_VERSION;
  dl.message_type = MESH_TYPE_ADAPTER_DATA;
  memcpy(dl.target_mac_address, leaf, 6);
  dl.route_len = 2;
  memcpy(&dl.route_path[0], r2.testDeviceMac(), 6); // R2
  memcpy(&dl.route_path[6], R1, 6);
  dl.epoch_num = 7;
  dl.seq_num = 3;

  resetEspNowMock();
  r2.processAdapterData(dl); // reachable directly — public under UNIT_TEST
  EXPECT_TRUE(wasSentTo(R1)) << "R2 forwards to route_path[1]=R1";
}

// (c) Last relay forwards to target_mac.
TEST_F(JoinAckRelayTest, DownlinkLastRelayForwardsToTarget) {
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  static constexpr uint8_t kR1[6] = {0x02, 0, 0, 0, 0, 0x11};
  Mesh r1 = makeIntermediateNodeWithMac(kR1); // R1
  mesh_message dl = {};
  dl.proto_version = PROTO_VERSION;
  dl.message_type = MESH_TYPE_ADAPTER_DATA;
  memcpy(dl.target_mac_address, leaf, 6);
  dl.route_len = 2;
  const uint8_t R2mac[6] = {0x02, 0, 0, 0, 0, 0x22};
  memcpy(&dl.route_path[0], R2mac, 6);
  memcpy(&dl.route_path[6], r1.testDeviceMac(), 6); // R1 at index 1 (last)
  dl.epoch_num = 7;
  dl.seq_num = 4;
  resetEspNowMock();
  r1.processAdapterData(dl);
  EXPECT_TRUE(wasSentTo(leaf)) << "last relay forwards to target_mac";
}

// Whole-branch-review finding: the downlink relay-forward branch never opens
// the sealed frame, so route_path[i+1] is attacker-controlled plaintext. An RF
// attacker can craft ADAPTER_DATA with this relay at route_path[i] and a fresh
// distinct MAC at route_path[i+1] on every frame (dodging replay dedup via
// fresh epoch/seq at the network entry point) to permanently grow the ESP-NOW
// peer table one entry per frame — spec §2's "20-peer cap, LRU-evicted" must
// bound this the same way Phase 2 bounded the uplink forwardingPeer.
TEST_F(JoinAckRelayTest, DownlinkRelayForward_BoundsAutoRegisteredPeers_NeverEvictsEnrolled) {
  Mesh mesh = makeIntermediateNode(); // kPeerMac is an ENROLLED peer — must never be evicted
  // Mirror what setupEspNow()/addPeer() do for a real enrolled peer at boot
  // (the fixture's peers.append() above only populates the PeerRegistry).
  MeshTransport::registerPeerWithEspNow(kPeerMac);

  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  const int kFloodCount = static_cast<int>(lattice::config::LATTICE_DOWNLINK_PEER_MAX) + 6;
  for (int i = 0; i < kFloodCount; ++i) {
    mesh_message dl{};
    dl.proto_version = PROTO_VERSION;
    dl.message_type = MESH_TYPE_ADAPTER_DATA;
    memcpy(dl.target_mac_address, leaf, 6);
    dl.route_len = 2;
    memcpy(&dl.route_path[0], kMyMac, 6); // this node is at route_path[0]
    // Fresh, distinct MAC at route_path[1] on every iteration — the attack.
    uint8_t nextMac[6] = {0x03, 0, 0, 0, static_cast<uint8_t>((i >> 8) & 0xFF),
                          static_cast<uint8_t>(i & 0xFF)};
    memcpy(&dl.route_path[6], nextMac, 6);
    dl.epoch_num = 1;
    dl.seq_num = static_cast<uint16_t>(i + 1);

    mesh.processAdapterData(dl);
  }

  EXPECT_LE(espNowRegisteredPeers.size(), 1 + lattice::config::LATTICE_DOWNLINK_PEER_MAX)
      << "auto-registered downlink forwarding peers must stay bounded by the LRU cap";
  EXPECT_TRUE(esp_now_is_peer_exist(kPeerMac))
      << "enrolled peer must never be evicted by downlink forwarding-peer churn";
}

// ─── sendDownlinkToNode clamp / registerDownlinkPeer LRU runtime guard ──────
// Defense-in-depth items from Phase E (issue #47 items 4 + 5).

// Not declared in any header — it's a plain (external-linkage) helper
// function defined next to sendDownlinkToNode() in Mesh.cpp, kept out of
// Mesh.h/RouteTable.h deliberately (this task's file list is Mesh.cpp +
// this test file only). Forward-declared here so the test below can call it.
namespace lattice {
namespace mesh {
bool downlinkRouteLenExceedsMaxHops(uint8_t pathLen);
}
}

// Item 4: sendDownlinkToNode() clamps pathLen against MAX_HOPS before
// indexing msg.route_path. RouteTable::record() already clamps pathLen to
// MAX_HOPS at write time (RouteTable.h: `if (pathLen > config::MAX_HOPS)
// return;`), and route-report processing rejects route_len > MAX_HOPS before
// it ever reaches RouteTable::record() (Mesh.cpp's processRouteReport, ~line
// 1175 pre-patch). Between those two guards there is no legitimate call path
// — and no public RouteTable API — that can hand routes->lookup() (and
// therefore sendDownlinkToNode) a pathLen > MAX_HOPS; RouteTable::Entry
// storage is private with no test hook to poke an oversized value directly,
// and adding one would mean touching RouteTable.h, outside this task's file
// list. So rather than trying to drive an integration path the codebase's
// own guards make unreachable, this exercises the extracted pure/stack-only
// predicate directly — the same one sendDownlinkToNode uses for the clamp.
TEST(MeshDownlinkClamp, OversizedRouteLen_Drops) {
  EXPECT_FALSE(lattice::mesh::downlinkRouteLenExceedsMaxHops(lattice::config::MAX_HOPS))
      << "pathLen == MAX_HOPS must NOT be flagged as oversized";
  EXPECT_TRUE(lattice::mesh::downlinkRouteLenExceedsMaxHops(lattice::config::MAX_HOPS + 1))
      << "pathLen == MAX_HOPS + 1 must be flagged as oversized (dropped)";
  EXPECT_TRUE(lattice::mesh::downlinkRouteLenExceedsMaxHops(0xFF))
      << "an arbitrary out-of-range pathLen must be flagged as oversized";
}

// Item 5: registerDownlinkPeer's enrolled/master short-circuit (the `if
// (peers.find(mac) || isCurrentMaster)` branch at the top of the function)
// fires on EVERY call once a MAC is enrolled/master, ahead of the
// LRU-touch loop further down — so that loop is unreachable for an
// already-enrolled MAC. The eviction of a stale LRU entry therefore has to
// happen inside the short-circuit branch itself (see the comment at
// Mesh.cpp::registerDownlinkPeer); this test asserts that actually happens,
// not just that the short-circuit continues to skip re-touching the LRU.
TEST(RegisterDownlinkPeer, LRUEntryBecomesEnrolled_EvictsOnTouch) {
  resetEspNowMock();
  lattice::mesh::Mesh m;
  uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  // 1. Register mac into the LRU (not yet enrolled/master).
  m.router.registerDownlinkPeer(mac, m.peers, m.currentMaster);
  EXPECT_EQ(m.router.downlinkPeerLruCount, 1u);

  // 2. mac becomes enrolled. PeerRegistry::peerMacs/peerCount/append() are
  //    all public (see PeerRegistry.h) — no addForTest hook exists or is
  //    needed; append() is the same public API other tests in this file use
  //    to seed peers directly (e.g. RelayedAdapterDataKeepsOriginTarget above).
  PeerInfo enrolled{};
  memcpy(enrolled.mac, mac, 6);
  enrolled.lastSeenMs = 0;
  ASSERT_TRUE(m.peers.append(enrolled));

  // 3. Call registerDownlinkPeer(mac) again.
  m.router.registerDownlinkPeer(mac, m.peers, m.currentMaster);

  // 4. LRU no longer contains mac — it was evicted, not just left untouched.
  EXPECT_EQ(m.router.downlinkPeerLruCount, 0u);
}

// ─── enrollPeer: secondary-master identity stamped into JOIN_ACK ────────────
// Helpers to inspect the broadcast JOIN_ACK by message_type — mirror
// wasSentTo/lastEspNowSentTo above, but keyed on the broadcast dest (FF:FF:…)
// + message_type rather than a unicast dest MAC.

static bool sawBroadcastOfType(uint8_t type) {
  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  for (auto it = espNowSentPackets.rbegin(); it != espNowSentPackets.rend(); ++it) {
    if (memcmp(it->addr, kBroadcast, 6) == 0 && it->data.size() >= sizeof(mesh_message)) {
      mesh_message m{};
      memcpy(&m, it->data.data(), sizeof(m));
      if (m.message_type == type)
        return true;
    }
  }
  return false;
}

static mesh_message lastEspNowBroadcastOfType(uint8_t type) {
  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  for (auto it = espNowSentPackets.rbegin(); it != espNowSentPackets.rend(); ++it) {
    if (memcmp(it->addr, kBroadcast, 6) == 0 && it->data.size() >= sizeof(mesh_message)) {
      mesh_message m{};
      memcpy(&m, it->data.data(), sizeof(m));
      if (m.message_type == type)
        return m;
    }
  }
  ADD_FAILURE() << "no broadcast ESP-NOW packet of the requested message_type was sent";
  return mesh_message{};
}

TEST_F(JoinAckRelayTest, EnrollPeerStampsSecondaryIdentityIntoJoinAck) {
  Mesh master = makeMasterNode();
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  uint8_t leafPub[32];
  for (int i = 0; i < 32; ++i) leafPub[i] = static_cast<uint8_t>(i + 1);
  const uint8_t sec[6] = {0x02, 0, 0, 0, 0, 0x02};
  uint8_t secPub[32];
  for (int i = 0; i < 32; ++i) secPub[i] = static_cast<uint8_t>(0x40 + i);

  resetEspNowMock();
  master.enrollPeer(leaf, leafPub, sec, secPub);

  // JOIN_ACK is broadcast; find it in the mock's sent frames.
  ASSERT_TRUE(sawBroadcastOfType(MESH_TYPE_JOIN_ACK));
  mesh_message ack = lastEspNowBroadcastOfType(MESH_TYPE_JOIN_ACK);
  // Protocol v0.6.0 (wire shrink §8): secondary master identity packed into
  // data[4..42] rather than top-level fields.
  EXPECT_EQ(0, memcmp(ack.data + 4, sec, 6));
  EXPECT_EQ(0, memcmp(ack.data + 10, secPub, 32));
}

TEST_F(JoinAckRelayTest, EnrollPeerTwoArgLeavesSecondaryZero) {
  Mesh master = makeMasterNode();
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  uint8_t leafPub[32] = {9};
  resetEspNowMock();
  master.enrollPeer(leaf, leafPub); // 2-arg: no secondary
  ASSERT_TRUE(sawBroadcastOfType(MESH_TYPE_JOIN_ACK));
  mesh_message ack = lastEspNowBroadcastOfType(MESH_TYPE_JOIN_ACK);
  uint8_t zero6[6] = {}, zero32[32] = {};
  EXPECT_EQ(0, memcmp(ack.data + 4, zero6, 6));
  EXPECT_EQ(0, memcmp(ack.data + 10, zero32, 32));
}

// ─── Config-opcode injection resistance (CRITICAL finding) ──────────────────
// Phase 3 seals+source-routes TARGETED downlink CONFIG_SET/NODE_ID_SET, and a
// node opens a self-addressed sealed ADAPTER_DATA with its k_down before
// honoring it (see the node-side E2E open block in processAdapterData). But a
// forged BROADCAST (target FF:FF:FF:FF:FF:FF) ADAPTER_DATA frame is never
// addressedToSelf, so it is never opened — it stays plaintext. Since
// origin_mac is attacker-controlled on the wire and the master's real MAC is
// public (broadcast in every beacon), a forged broadcast frame with
// origin_mac spoofed to the master passes the origin check too. Without a
// guard, this reaches externalRecvCallback (-> Adapter::onMeshData ->
// ESP.restart()) completely unauthenticated: one plaintext RF frame reboots
// or reconfigures any node.

class ConfigOpcodeInjectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
    lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  }

  static constexpr uint8_t kMyMac[6] = {0x02, 0, 0, 0, 0, 0x0B};
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  uint8_t nodePriv[32], nodePub[32];
  uint8_t masterPriv[32], masterPub[32];

  // A non-master node enrolled under kMasterMac with real Curve25519 keys, so
  // masterE2EKeys() can derive the same k_down the master would use to seal a
  // legitimate targeted downlink.
  Mesh makeEnrolledNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false;
    memcpy(mesh.enrollment.devicePrivateKey, nodePriv, 32);
    memcpy(mesh.enrollment.devicePublicKey, nodePub, 32);
    mesh.enrollment.hasMasterMac = true;
    memcpy(mesh.enrollment.knownMasterMac, kMasterMac, 6);
    memcpy(mesh.currentMaster.mac, kMasterMac, 6);
    mesh.currentMaster.distance = 1;
    PeerInfo master{};
    memcpy(master.mac, kMasterMac, 6);
    memcpy(master.publicKey, masterPub, 32);
    master.lastSeenMs = 0;
    mesh.peers.append(master);
    return mesh;
  }

  // Exactly the attack in the finding: BROADCAST target, SERIAL_ADAPTER
  // data_type, data[0]=OP_CONFIG_SET, data[1..6]=victim's own MAC,
  // origin_mac spoofed to the master's public MAC, NOT sealed.
  mesh_message makeForgedBroadcastConfigSet() {
    mesh_message m{};
    m.proto_version = PROTO_VERSION;
    m.message_type = MESH_TYPE_ADAPTER_DATA;
    m.data_type = adapter_types::SERIAL_ADAPTER;
    memcpy(m.origin_mac_address, kMasterMac, 6); // spoofed: master's public MAC
    memcpy(m.target_mac_address, kBroadcastMac, 6); // forged: broadcast, never opened
    memcpy(m.last_hop_mac_address, kMasterMac, 6);
    m.epoch_num = 1;
    m.seq_num = 1;
    m.data[0] = OP_CONFIG_SET;
    memcpy(&m.data[1], kMyMac, 6); // victim's own MAC as the opcode's target field
    m.data[7] = 0x02;              // attacker-chosen adapter type
    // NOT sealed — plaintext, exactly as an RF attacker would send it.
    return m;
  }
};

constexpr uint8_t ConfigOpcodeInjectionTest::kMyMac[];
constexpr uint8_t ConfigOpcodeInjectionTest::kMasterMac[];
constexpr uint8_t ConfigOpcodeInjectionTest::kBroadcastMac[];

TEST_F(ConfigOpcodeInjectionTest, ForgedBroadcastConfigSet_NotDeliveredToExternalCallback) {
  Mesh mesh = makeEnrolledNode();
  bool sawConfigSet = false;
  mesh.linkDataRecvCallback([&](const mesh_message& m) {
    if (m.data_type == adapter_types::SERIAL_ADAPTER && m.data[0] == OP_CONFIG_SET)
      sawConfigSet = true;
  });

  mesh.processAdapterData(makeForgedBroadcastConfigSet());

  EXPECT_FALSE(sawConfigSet)
      << "a forged plaintext BROADCAST CONFIG_SET must never reach externalRecvCallback — "
         "it was never addressedToSelf, so it was never opened/authenticated with k_down";
}

TEST_F(ConfigOpcodeInjectionTest, ForgedBroadcastNodeIdSet_NotDeliveredToExternalCallback) {
  Mesh mesh = makeEnrolledNode();
  int deliveredCount = 0;
  mesh.linkDataRecvCallback([&](const mesh_message&) { ++deliveredCount; });

  mesh_message forged = makeForgedBroadcastConfigSet();
  forged.data[0] = OP_NODE_ID_SET;
  forged.data[7] = 0x42; // attacker-chosen node ID

  mesh.processAdapterData(forged);

  EXPECT_EQ(deliveredCount, 0)
      << "a forged plaintext BROADCAST NODE_ID_SET must never reach externalRecvCallback";
}

// Legitimate counterpart: a genuinely targeted, sealed CONFIG_SET
// (addressedToSelf, opened with k_down) must still be delivered — the guard
// above must not overreach and break the real downlink path.
TEST_F(ConfigOpcodeInjectionTest, TargetedSealedConfigSet_StillDelivered) {
  Mesh mesh = makeEnrolledNode();
  const uint8_t *kUp, *kDown;
  ASSERT_TRUE(mesh.masterE2EKeys(&kUp, &kDown));

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::SERIAL_ADAPTER;
  memcpy(msg.origin_mac_address, kMasterMac, 6);
  memcpy(msg.target_mac_address, kMyMac, 6); // targeted, not broadcast
  memcpy(msg.last_hop_mac_address, kMasterMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 1;
  msg.data[0] = OP_CONFIG_SET;
  memcpy(&msg.data[1], kMyMac, 6);
  msg.data[7] = 0x02;
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kDown, msg));

  bool sawConfigSet = false;
  mesh.linkDataRecvCallback([&](const mesh_message& m) {
    if (m.data_type == adapter_types::SERIAL_ADAPTER && m.data[0] == OP_CONFIG_SET)
      sawConfigSet = true;
  });

  mesh.processAdapterData(msg);

  EXPECT_TRUE(sawConfigSet) << "a genuinely targeted, sealed CONFIG_SET (addressedToSelf, "
                               "opened with k_down) must still be delivered";
}

// ─── JOIN_ACK forgery resistance ─────────────────────────────────────────────
// JOIN_ACKs travel over the unencrypted broadcast peer, and everything a forger
// needs is observable over the air: the victim's pubkey prefix (broadcast in its
// own ENROLLMENT requests) and the master's MAC + pubkey (broadcast in every
// legitimate JOIN_ACK). The fingerprint check alone therefore does NOT
// authenticate the sender — the registration path must additionally be gated by
// TOFU origin and must never replace established key material.
//
// These tests exercise that defense-in-depth (origin gate / no-rekey) in
// isolation, independent of the Phase D (#42) master pubkey pin: forged ACKs
// here carry an attacker-generated key (attackerKey), which a real attacker
// necessarily uses since it doesn't have the master's private key — the pin
// alone would already reject every case below before the origin gate ever
// runs. The pin bypass keeps this fixture actually exercising the origin/
// no-rekey code paths rather than trivially passing on the pin check.
class JoinAckForgeryTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
    lattice::mesh::crypto::generateKeypair(masterPriv, masterKey);
    lattice::mesh::crypto::generateKeypair(attackerPriv, attackerKey);
    lattice::mesh::pin::setTestBypass(true);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kAttackerMac[6] = {0xBA, 0xDB, 0xAD, 0xBA, 0xDB, 0xAD};

  uint8_t nodePriv[32], nodePub[32];
  uint8_t masterPriv[32], masterKey[32];
  uint8_t attackerPriv[32], attackerKey[32];

  // A node that already trusts kMasterMac: TOFU MAC recorded, master registered
  // as a peer with an established (non-zero) key.
  Mesh makeEnrolledNode() {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    mesh.isMaster = false;
    memcpy(mesh.enrollment.devicePrivateKey, nodePriv, 32);
    memcpy(mesh.enrollment.devicePublicKey, nodePub, 32);
    mesh.enrollment.hasMasterMac = true;
    memcpy(mesh.enrollment.knownMasterMac, kMasterMac, 6);
    PeerInfo master{};
    memcpy(master.mac, kMasterMac, 6);
    memcpy(master.publicKey, masterKey, 32);
    master.lastSeenMs = 0;
    mesh.peers.append(master);
    return mesh;
  }

  // Forged JOIN_ACK addressed to the victim, carrying the victim's (observable)
  // fingerprint and attacker-chosen key material.
  mesh_message makeForgedAck(const uint8_t origin[6]) {
    mesh_message m{};
    m.proto_version = 1;
    m.message_type = MESH_TYPE_JOIN_ACK;
    m.data_type = adapter_types::UNKNOWN_ADAPTER;
    memcpy(m.origin_mac_address, origin, 6);
    memcpy(m.target_mac_address, kMyMac, 6);
    memcpy(m.last_hop_mac_address, origin, 6);
    m.hop_count = 1;
    m.epoch_num = 1;
    m.seq_num = 1;
    memcpy(m.data, nodePub, 4); // victim fingerprint — observable over the air
    memcpy(m.enrollment_public_key, attackerKey, 32);
    return m;
  }
};

constexpr uint8_t JoinAckForgeryTest::kMyMac[];
constexpr uint8_t JoinAckForgeryTest::kMasterMac[];
constexpr uint8_t JoinAckForgeryTest::kAttackerMac[];

TEST_F(JoinAckForgeryTest, UnexpectedOrigin_DroppedEntirely) {
  Mesh mesh = makeEnrolledNode();

  mesh.processJoinAck(makeForgedAck(kAttackerMac));

  EXPECT_EQ(mesh.peers.find(kAttackerMac), nullptr)
      << "a JOIN_ACK from a non-master origin must not register that origin as a peer";
  PeerInfo* master = mesh.peers.find(kMasterMac);
  ASSERT_NE(master, nullptr);
  EXPECT_EQ(memcmp(master->publicKey, masterKey, 32), 0)
      << "known master's established key must be untouched";
  EXPECT_FALSE(mesh.enrollment.isEnrolled())
      << "a JOIN_ACK from an unexpected origin must not mark the node enrolled";
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, kMasterMac, 6), 0)
      << "TOFU master MAC must be untouched";
}

TEST_F(JoinAckForgeryTest, SpoofedMasterOrigin_DoesNotRekeyEstablishedMasterPeer) {
  Mesh mesh = makeEnrolledNode();

  // Attacker spoofs the (observable) known-master MAC as origin, supplying its
  // own key: the origin gate alone cannot catch this — the registration path
  // must refuse to replace already-established key material.
  mesh.processJoinAck(makeForgedAck(kMasterMac));

  PeerInfo* master = mesh.peers.find(kMasterMac);
  ASSERT_NE(master, nullptr);
  EXPECT_EQ(memcmp(master->publicKey, masterKey, 32), 0)
      << "an over-the-air JOIN_ACK must never re-key an established master peer";
}

TEST_F(JoinAckForgeryTest, MasterNode_IgnoresJoinAckAddressedToItself) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = true;
  memcpy(mesh.enrollment.devicePrivateKey, nodePriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, nodePub, 32);
  // A fresh master has no TOFU master MAC — without an isMaster guard a forged
  // ACK could TOFU-poison it and register attacker key material.

  mesh.processJoinAck(makeForgedAck(kAttackerMac));

  EXPECT_EQ(mesh.peers.find(kAttackerMac), nullptr)
      << "a master must not peer-register from a JOIN_ACK addressed to itself";
  EXPECT_FALSE(mesh.enrollment.hasMasterMac)
      << "a master must not TOFU-learn a 'master' from a forged JOIN_ACK";
  EXPECT_FALSE(mesh.enrollment.isEnrolled()) << "masters never enroll via JOIN_ACK";
}

// ─── drain(): replay protection ──────────────────────────────────────────────
// Relay dedup is Mesh::handleReceivedMessage's responsibility (reached via
// transport.drain(), Phase B Task 4 — formerly Mesh::drainRecvQueue's inline
// body); relay paths no longer call isReplay directly. These tests verify
// that the production path (drain() → handleReceivedMessage → dispatch)
// correctly drops replayed messages before handlers are invoked.

class DrainRecvQueueTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kOriginMac[6] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};

  void injectAndDrain(Mesh& mesh, const mesh_message& msg) {
    MeshTransport::RecvQueueEntry entry;
    memcpy(&entry.msg, &msg, sizeof(msg));
    memcpy(entry.srcMac, msg.origin_mac_address, 6);
    xRingbufferSend(mesh.transport.recvQueue, &entry, sizeof(entry), 0);
    mesh.drain();
  }
};

constexpr uint8_t DrainRecvQueueTest::kMyMac[];
constexpr uint8_t DrainRecvQueueTest::kOriginMac[];

TEST_F(DrainRecvQueueTest, DropsReplayedAdapterData) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = true; // master: delivers locally, no relay — clean baseline

  // Task 6 (E2E AEAD): master opens sealed ADAPTER_DATA before dispatch, so
  // this frame must be sealed with keys the master can actually derive.
  uint8_t masterPriv[32], masterPub[32], originPriv[32], originPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo origin{};
  memcpy(origin.mac, kOriginMac, 6);
  memcpy(origin.publicKey, originPub, 32);
  origin.lastSeenMs = 0;
  mesh.peers.append(origin);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);

  int deliveredCount = 0;
  mesh.linkDataRecvCallback([&](const mesh_message&) { ++deliveredCount; });

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kOriginMac, 6);
  memcpy(msg.target_mac_address, kMyMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 42;
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  injectAndDrain(mesh, msg); // first: not replay — delivered
  EXPECT_EQ(deliveredCount, 1);

  injectAndDrain(mesh, msg);    // replay: handleReceivedMessage drops before dispatch
  EXPECT_EQ(deliveredCount, 1); // callback not invoked again
}

// Final-review fix: proto_version == 0 must not bypass the flag-day version
// drop, and (since the replay gate is keyed on proto_version == PROTO_VERSION)
// must also not bypass replay dedup. Everything else about this frame is
// otherwise valid (properly sealed, addressed to self, fresh epoch/seq) so the
// only reason it can be rejected is the version check itself.
TEST_F(DrainRecvQueueTest, DropsProtoVersionZeroAdapterData) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = true;

  uint8_t masterPriv[32], masterPub[32], originPriv[32], originPub[32];
  lattice::mesh::crypto::generateKeypair(masterPriv, masterPub);
  lattice::mesh::crypto::generateKeypair(originPriv, originPub);
  memcpy(mesh.enrollment.devicePrivateKey, masterPriv, 32);
  memcpy(mesh.enrollment.devicePublicKey, masterPub, 32);
  PeerInfo origin{};
  memcpy(origin.mac, kOriginMac, 6);
  memcpy(origin.publicKey, originPub, 32);
  origin.lastSeenMs = 0;
  mesh.peers.append(origin);
  uint8_t kUp[32], kDown[32];
  lattice::mesh::crypto::deriveE2EKeys(originPriv, masterPub, kUp, kDown);

  int deliveredCount = 0;
  mesh.linkDataRecvCallback([&](const mesh_message&) { ++deliveredCount; });

  mesh_message msg{};
  msg.proto_version = 0; // forged/malformed — must be dropped, not delivered
  msg.message_type = MESH_TYPE_ADAPTER_DATA;
  msg.data_type = adapter_types::PIR_ADAPTER;
  memcpy(msg.origin_mac_address, kOriginMac, 6);
  memcpy(msg.target_mac_address, kMyMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 99;
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(kUp, msg));

  injectAndDrain(mesh, msg);
  EXPECT_EQ(deliveredCount, 0); // dropped by the version check, never reaches dispatch
}

// Task 9c R1: an enrollment request seen twice within one drain window (e.g. the
// direct broadcast plus a neighbour's relayed copy, same origin/epoch/seq) must
// be forwarded to the server only once — the master enqueues one pending relay.
TEST_F(DrainRecvQueueTest, DropsReplayedEnrollmentRequestBeforeRelay) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = true;

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ENROLLMENT;
  memcpy(msg.origin_mac_address, kOriginMac, 6);
  msg.epoch_num = 1;
  msg.seq_num = 7;

  injectAndDrain(mesh, msg); // first: enqueued for relay-to-server
  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 1u);
  injectAndDrain(mesh, msg); // duplicate (same epoch/seq): dropped before processRequest
  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 1u)
      << "duplicate must not enqueue a second relay";
}

// Task 9c R1 (retry preservation): a legitimate re-request in a LATER retry round
// carries a fresh seq, so dedup must NOT suppress it — it enqueues again.
TEST_F(DrainRecvQueueTest, ForwardsEnrollmentRetryWithFreshSeq) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = true;

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ENROLLMENT;
  memcpy(msg.origin_mac_address, kOriginMac, 6);
  msg.epoch_num = 1;

  msg.seq_num = 7;
  injectAndDrain(mesh, msg);
  msg.seq_num = 8; // next 10s retry round — distinct seq
  injectAndDrain(mesh, msg);
  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 2u) << "retry must still be forwarded";
}

// Task 9c R2: a node re-broadcasts a given JOIN_ACK at most once. A reflected copy
// (same origin/epoch/seq) is dropped by ReplayCache before processJoinAck, so no
// second broadcast — preventing combinatorial breadth amplification.
TEST_F(DrainRecvQueueTest, DoesNotReBroadcastReplayedJoinAck) {
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t kDistant[6] = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44};
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  mesh.isMaster = false;

  mesh_message ack{};
  ack.proto_version = PROTO_VERSION;
  ack.message_type = MESH_TYPE_JOIN_ACK;
  memcpy(ack.origin_mac_address, kMasterMac, 6); // originated by master, not us
  memcpy(ack.target_mac_address, kDistant, 6);   // addressed elsewhere -> relay branch
  ack.hop_count = 1;
  ack.epoch_num = 1;
  ack.seq_num = 9;

  size_t before = espNowSentPackets.size();
  injectAndDrain(mesh, ack); // first: re-broadcast once
  injectAndDrain(mesh, ack); // reflected duplicate: dropped before processJoinAck
  EXPECT_EQ(espNowSentPackets.size(), before + 1) << "same JOIN_ACK re-broadcast at most once";
}

// ─── EnrollmentTest ──────────────────────────────────────────────────────────

class EnrollmentTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    espNowSentPackets.clear();
  }
};

TEST_F(EnrollmentTest, SendsSingleEspNowMessage) {
  Mesh mesh;
  static constexpr uint8_t kPubKey[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                          0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                          0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                          0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
  memcpy(mesh.enrollment.devicePublicKey, kPubKey, 32);

  mesh.sendEnrollmentRequest();

  ASSERT_EQ(espNowSentPackets.size(), 1u)
      << "Expected exactly 1 ESP-NOW packet (was 3 with old chunking)";
  const auto& pkt = espNowSentPackets[0];
  ASSERT_GE(pkt.data.size(), sizeof(mesh_message));
  const mesh_message* sent = reinterpret_cast<const mesh_message*>(pkt.data.data());
  EXPECT_EQ(sent->message_type, MESH_TYPE_ENROLLMENT);
  EXPECT_EQ(memcmp(sent->enrollment_public_key, kPubKey, 32), 0)
      << "Full public key must be present in a single message";
}

// Task 9c R1: the enrollment request must carry proto_version + (epoch, seq) so
// the master's ReplayCache dedups relayed copies; and each retry round must use a
// FRESH seq so a legitimate re-request is not permanently suppressed.
TEST_F(EnrollmentTest, EnrollmentRequestCarriesReplayFieldsWithFreshSeqPerRetry) {
  Mesh mesh;
  mesh.txState.init(5); // bootEpoch = 5 (> 0, so the handleReceivedMessage replay gate applies)

  mesh.sendEnrollmentRequest();
  mesh.sendEnrollmentRequest(); // next retry round

  ASSERT_EQ(espNowSentPackets.size(), 2u);
  const auto* m1 = reinterpret_cast<const mesh_message*>(espNowSentPackets[0].data.data());
  const auto* m2 = reinterpret_cast<const mesh_message*>(espNowSentPackets[1].data.data());
  EXPECT_EQ(m1->proto_version, PROTO_VERSION);
  EXPECT_EQ(m1->epoch_num, 5u);
  EXPECT_GT(m1->seq_num, 0u);
  EXPECT_NE(m1->seq_num, m2->seq_num) << "each retry round must use a fresh seq";
}

TEST_F(EnrollmentTest, ProcessSingleMessageSetsKey) {
  Mesh mesh;
  mesh.isMaster = true;

  mesh_message msg = {};
  msg.message_type = MESH_TYPE_ENROLLMENT;
  static constexpr uint8_t kMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  static constexpr uint8_t kKey[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                       0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                       0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
  memcpy(msg.origin_mac_address, kMac, 6);
  memcpy(msg.enrollment_public_key, kKey, 32);

  mesh.enrollment.processRequest(msg);

  ASSERT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 1u);
  const auto& e = *reinterpret_cast<const PendingRelayQueue::Entry*>(
      mesh.enrollment._relayQueue._queue->items.front().data());
  EXPECT_EQ(memcmp(e.mac, kMac, 6), 0);
  EXPECT_EQ(memcmp(e.pubKey, kKey, 32), 0)
      << "Full 32-byte key must be copied without chunk reassembly";
}

// ─── EnrollmentPinTest ────────────────────────────────────────────────────────
// Phase D (#42): Enrollment::processJoinAck now requires the JOIN_ACK's
// enrollment_public_key to match the compile-time-pinned lattice::mesh::pin::
// MASTER_PUBKEY before it will register the sender or TOFU-learn its MAC. This
// closes the enrollment-instant MITM window on the pubkey side — an RF-present
// attacker without the master's private key cannot forge a JOIN_ACK the pin
// will accept.

class EnrollmentPinTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    // Ensure pin check is active for every test in this fixture unless the
    // test explicitly bypasses.
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }
};

TEST_F(EnrollmentPinTest, ProcessJoinAck_ValidPubkey_Enrolls) {
  // JOIN_ACK whose enrollment_public_key matches the pinned test value must
  // be accepted: the node registers the master's MAC via TOFU.
  mesh_message ack{};
  ack.message_type = MESH_TYPE_JOIN_ACK;
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  memcpy(ack.origin_mac_address, kMasterMac, 6);
  // fingerprint field left zeroed to match Enrollment's default (zeroed)
  // devicePublicKey — msg.data untouched.
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);

  Enrollment enrollment;
  enrollment.processJoinAck(ack, /*deviceMac*/ nullptr, /*registerFn*/ nullptr);

  EXPECT_TRUE(enrollment.hasMasterMac);
  EXPECT_EQ(memcmp(enrollment.knownMasterMac, kMasterMac, 6), 0);
}

TEST_F(EnrollmentPinTest, ProcessJoinAck_WrongPubkey_DropsNoEnroll) {
  // A JOIN_ACK whose enrollment_public_key does NOT match the pin must be
  // dropped before any state mutation — no TOFU-learn, no enrolled flag.
  mesh_message ack{};
  ack.message_type = MESH_TYPE_JOIN_ACK;
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  memcpy(ack.origin_mac_address, kMasterMac, 6);
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);
  ack.enrollment_public_key[0] ^= 0xFF; // corrupt first byte — mismatches pin

  Enrollment enrollment;
  enrollment.processJoinAck(ack, /*deviceMac*/ nullptr, /*registerFn*/ nullptr);

  EXPECT_FALSE(enrollment.hasMasterMac);
  EXPECT_FALSE(enrollment.isEnrolled());
}

TEST_F(EnrollmentPinTest, ProcessJoinAck_TestBypass_SkipsCheck) {
  // The UNIT_TEST-only runtime bypass simulates DEV_MODE=true (which is a
  // compile-time constant and can't be flipped from a test): with the bypass
  // active, a wrong pubkey must still enrol.
  lattice::mesh::pin::setTestBypass(true);
  mesh_message ack{};
  ack.message_type = MESH_TYPE_JOIN_ACK;
  static constexpr uint8_t kMasterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  memcpy(ack.origin_mac_address, kMasterMac, 6);
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);
  ack.enrollment_public_key[0] ^= 0xFF; // wrong — but bypass active

  Enrollment enrollment;
  enrollment.processJoinAck(ack, /*deviceMac*/ nullptr, /*registerFn*/ nullptr);

  EXPECT_TRUE(enrollment.hasMasterMac);
}

// ---- EnrollmentRelayCallbackTest ----

// Phase I Task 8 (item OO): the ring buffer's item memory is only valid for
// the duration of the drainPendingRelay() callback — vRingbufferReturnItem()
// (called right after the callback returns) frees/recycles it, unlike the
// old array-backed queue where a drained slot's bytes stayed live
// indefinitely. Production callbacks already only ever copy synchronously
// (see SerialAdapter::relayEnrollmentToServer's memcpy) rather than retain
// the pointer, so this test helper must do the same — copy into static
// storage here rather than stash the raw pointer for post-drain inspection.
static bool g_captured = false;
static uint8_t g_capturedMac[6];
static uint8_t g_capturedKey[32];

static void captureRelayFn(const uint8_t mac[6], const uint8_t pubKey[32]) {
  g_captured = true;
  memcpy(g_capturedMac, mac, 6);
  memcpy(g_capturedKey, pubKey, 32);
}

class EnrollmentRelayCallbackTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_captured = false;
    memset(g_capturedMac, 0, sizeof(g_capturedMac));
    memset(g_capturedKey, 0, sizeof(g_capturedKey));
    EEPROM.reset();
  }
};

TEST_F(EnrollmentRelayCallbackTest, DrainCallsRegisteredCallback) {
  Mesh mesh;
  mesh.isMaster = true;
  mesh.setEnrollmentRelayFn(captureRelayFn);

  static constexpr uint8_t kMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kKey[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                       0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                       0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};

  mesh.enrollment.setPendingRelay(kMac, kKey);

  mesh.enrollment.drainPendingRelay();

  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 0u) << "queue must be empty after drain";
  ASSERT_TRUE(g_captured) << "callback was not called";
  EXPECT_EQ(memcmp(g_capturedMac, kMac, 6), 0) << "wrong MAC passed to callback";
  EXPECT_EQ(memcmp(g_capturedKey, kKey, 32), 0) << "wrong pubKey passed to callback";
}

TEST_F(EnrollmentRelayCallbackTest, DrainWithNoCallbackClearsFlag) {
  Mesh mesh;
  mesh.isMaster = true;
  // No callback registered.

  static constexpr uint8_t kMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  static constexpr uint8_t kKey[32] = {0};
  mesh.enrollment.setPendingRelay(kMac, kKey);
  mesh.enrollment.drainPendingRelay();

  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 0u)
      << "queue must clear even with no callback";
  EXPECT_FALSE(g_captured) << "callback must not fire when unregistered";
}

// Bug #6 regression: two enrollment requests queued before a single drain must
// BOTH be relayed. The old single-slot latch dropped the first (only the last
// survived), starving a concurrently-enrolling node.
TEST_F(EnrollmentRelayCallbackTest, QueueHoldsAndDrainsMultipleConcurrentRelays) {
  Mesh mesh;
  mesh.isMaster = true;

  static std::vector<std::array<uint8_t, 6>> drained;
  drained.clear();
  mesh.setEnrollmentRelayFn(
      [](const uint8_t mac[6], const uint8_t /*pubKey*/[32]) {
        std::array<uint8_t, 6> m{};
        memcpy(m.data(), mac, 6);
        drained.push_back(m);
      });

  static constexpr uint8_t kMacA[6] = {0xA0, 0, 0, 0, 0, 0x0A};
  static constexpr uint8_t kMacB[6] = {0xB0, 0, 0, 0, 0, 0x0B};
  uint8_t key[32] = {0x11};

  mesh_message reqA{};
  reqA.message_type = MESH_TYPE_ENROLLMENT;
  memcpy(reqA.origin_mac_address, kMacA, 6);
  memcpy(reqA.enrollment_public_key, key, 32);
  mesh_message reqB = reqA;
  memcpy(reqB.origin_mac_address, kMacB, 6);

  mesh.enrollment.processRequest(reqA);
  mesh.enrollment.processRequest(reqB); // second request must NOT overwrite the first
  ASSERT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 2u);

  mesh.enrollment.drainPendingRelay();

  ASSERT_EQ(drained.size(), 2u) << "both queued relays must fire (Bug #6 starvation)";
  EXPECT_EQ(memcmp(drained[0].data(), kMacA, 6), 0) << "FIFO order: A first";
  EXPECT_EQ(memcmp(drained[1].data(), kMacB, 6), 0) << "FIFO order: B second";
  EXPECT_EQ(mesh.enrollment._relayQueue._queue->items.size(), 0u);
}

// --- Seal-time AEAD epoch-rollback guard (Phase A Task 3) ---

class MeshEpochRollbackTest : public ::testing::Test {
protected:
  lattice::mesh::Mesh mesh;
  void SetUp() override { /* mesh default-constructed; _lastSealedEpoch = UINT32_MAX */ }
};

TEST_F(MeshEpochRollbackTest, FirstCall_Snapshots) {
  EXPECT_NO_THROW(mesh._checkEpochRollback(3, 7));
}

TEST_F(MeshEpochRollbackTest, HigherEpoch_Passes) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_NO_THROW(mesh._checkEpochRollback(4, 0));
}

TEST_F(MeshEpochRollbackTest, SameEpochHigherSeq_Passes) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_NO_THROW(mesh._checkEpochRollback(3, 8));
}

TEST_F(MeshEpochRollbackTest, LowerEpoch_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(2, 0), lattice::err::FatalError);
}

TEST_F(MeshEpochRollbackTest, SameEpochLowerSeq_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(3, 6), lattice::err::FatalError);
}

TEST_F(MeshEpochRollbackTest, SameEpochSameSeq_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(3, 7), lattice::err::FatalError);
}

// --- currentMaster.distance derivation from NeighborTable (issue #45) ---
//
// In UNIT_TEST builds all of Mesh's members are public (see the
// `#ifdef UNIT_TEST public: #else private: #endif` at the top of the class
// body in Mesh.h), so test bodies read/set `mesh.enrollment` and
// `mesh.currentMaster` directly — same pattern MeshLogicTest and
// MeshEpochRollbackTest above already use (e.g. `mesh._checkEpochRollback`,
// `mesh.enrollment.hasMasterMac`). No `_enrollmentForTest()` /
// `_currentMasterForTest()` accessors or `friend class` declaration needed.
class MeshDistanceDerivationTest : public ::testing::Test {
protected:
  lattice::mesh::Mesh mesh;
  void SetUp() override { resetMillis(); }
};

TEST_F(MeshDistanceDerivationTest, DirectBeacon_DistanceIs1) {
  // Build a beacon: hop_count=0, last_hop=master, origin=master
  mesh_message m{};
  // Origin MAC must equal the pinned master MAC (Phase D, #42) or the beacon
  // pin check drops it before distance derivation ever runs.
  const uint8_t* master = lattice::mesh::pin::MASTER_MAC;
  memcpy(m.origin_mac_address, master, 6);
  memcpy(m.last_hop_mac_address, master, 6);
  m.message_type = MESH_TYPE_MASTER_BEACON;
  m.hop_count = 0;
  // Set enrollment.knownMasterMac so the TOFU fromPrimary branch accepts.
  memcpy(mesh.enrollment.knownMasterMac, master, 6);
  mesh.enrollment.hasMasterMac = true;
  mesh.beacon.process(m, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 1);
}

TEST_F(MeshDistanceDerivationTest, SinglePathAgeOut_DistanceRises) {
  // Origin MAC must equal the pinned master MAC (Phase D, #42) or the beacon
  // pin check drops it before distance derivation ever runs.
  const uint8_t* master = lattice::mesh::pin::MASTER_MAC;
  const uint8_t relay[6] = {0xAA, 0, 0, 0, 0, 2};
  memcpy(mesh.enrollment.knownMasterMac, master, 6);
  mesh.enrollment.hasMasterMac = true;

  // First beacon direct from master → distance=1.
  mesh_message direct{};
  memcpy(direct.origin_mac_address, master, 6);
  memcpy(direct.last_hop_mac_address, master, 6);
  direct.message_type = MESH_TYPE_MASTER_BEACON;
  direct.hop_count = 0;
  mesh.beacon.process(direct, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  ASSERT_EQ(mesh.currentMaster.distance, 1);

  // Advance clock past STALE_PEER_THRESHOLD_MS — the direct master neighbor
  // entry ages out.
  advanceMillis(lattice::config::STALE_PEER_THRESHOLD_MS + 1);

  // Second beacon via a relay at distance 2 from the master (still the same
  // origin master MAC, just heard via a longer path) → distance=3.
  mesh_message relayed{};
  memcpy(relayed.origin_mac_address, master, 6);
  memcpy(relayed.last_hop_mac_address, relay, 6);
  relayed.message_type = MESH_TYPE_MASTER_BEACON;
  relayed.hop_count = 2;
  mesh.beacon.process(relayed, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 3);
}

TEST_F(MeshDistanceDerivationTest, TwoPathsDifferentLength_NoOscillation) {
  // Origin MAC must equal the pinned master MAC (Phase D, #42) or the beacon
  // pin check drops it before distance derivation ever runs.
  const uint8_t* master = lattice::mesh::pin::MASTER_MAC;
  const uint8_t relay[6] = {0xAA, 0, 0, 0, 0, 2};
  memcpy(mesh.enrollment.knownMasterMac, master, 6);
  mesh.enrollment.hasMasterMac = true;

  mesh_message direct{};
  memcpy(direct.origin_mac_address, master, 6);
  memcpy(direct.last_hop_mac_address, master, 6);
  direct.message_type = MESH_TYPE_MASTER_BEACON;
  direct.hop_count = 0;

  mesh_message relayed{};
  memcpy(relayed.origin_mac_address, master, 6);
  memcpy(relayed.last_hop_mac_address, relay, 6);
  relayed.message_type = MESH_TYPE_MASTER_BEACON;
  relayed.hop_count = 2;

  // Interleave direct + relayed beacons while both neighbor entries stay
  // fresh — the direct (distance-0) neighbor always wins minFreshDistance,
  // so currentMaster.distance must stay 1 throughout. No oscillation.
  mesh.beacon.process(direct, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 1);

  advanceMillis(10);
  mesh.beacon.process(relayed, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 1) << "shorter path still fresh — must not rise";

  advanceMillis(10);
  mesh.beacon.process(direct, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 1);

  advanceMillis(10);
  mesh.beacon.process(relayed, mesh.deviceMacAddress, mesh.isMaster, mesh._dualMasterMode, mesh.enrollment, mesh.neighbors, mesh.currentMaster, mesh.txState, mesh.relayPendingMsg, mesh.relayPendingAt, mesh.relayPending, mesh.lastSeenMasterMac);
  EXPECT_EQ(mesh.currentMaster.distance, 1) << "must not oscillate";
}

// Task 3 (issue #51): RouteTable is heap-allocated only when this node is a
// master, freed on demotion — leaves must never pay its ~2.25 KB static RAM.
class MeshRouteTableAllocationTest : public ::testing::Test {
protected:
  void SetUp() override { resetMillis(); }
};

TEST_F(MeshRouteTableAllocationTest, LeafRole_RoutesIsNullptr) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(false);
  mesh.init();
  EXPECT_EQ(mesh.testRoutes(), nullptr);
}

TEST_F(MeshRouteTableAllocationTest, MasterPromotion_AllocatesRoutes) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(true);
  mesh.init();
  EXPECT_NE(mesh.testRoutes(), nullptr);
}

TEST_F(MeshRouteTableAllocationTest, MasterDemotion_FreesRoutes) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(true);
  mesh.init();
  ASSERT_NE(mesh.testRoutes(), nullptr);
  mesh.setIsMaster(false);
  mesh.reevaluateRouteTable(); // new helper: honours current isMaster state
  EXPECT_EQ(mesh.testRoutes(), nullptr);
}
