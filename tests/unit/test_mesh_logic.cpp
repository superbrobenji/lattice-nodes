#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "mesh/Mesh.h"
#include "mesh/MeshCrypto.h"
#include "mesh/E2ECrypto.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "lib/lattice-protocol/c/opcodes.h"

using namespace lattice::mesh;

class MeshLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
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

  mesh.processMasterBeacon(beacon);

  EXPECT_TRUE(mesh.enrollment.hasMasterMac);
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, masterMac, 6), 0);
}

TEST_F(MeshLogicTest, TOFU_SecondBeaconFromSameMAC_Accepted) {
  Mesh mesh;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 1));

  // Second beacon from same MAC — should update lastMasterSeenMs, not reject
  advanceMillis(3000);
  auto beacon2 = makeBeacon(masterMac, 1, 2);
  // No assertion — just verify no crash and relay fires
  mesh.processMasterBeacon(beacon2);
  EXPECT_TRUE(mesh.enrollment.hasMasterMac);
}

TEST_F(MeshLogicTest, TOFU_BeaconFromImpostorMAC_Rejected_WhenMasterAlive) {
  Mesh mesh;
  const uint8_t realMaster[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t impostorMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

  // Learn real master
  mesh.processMasterBeacon(makeBeacon(realMaster, 1, 1));

  // Impostor beacon arrives while real master is still fresh
  size_t sendsBefore = espNowSentPackets.size();
  mesh.processMasterBeacon(makeBeacon(impostorMac, 1, 2));

  // Impostor should NOT be accepted as master
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, realMaster, 6), 0);
  // Relay should NOT fire for impostor
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}

TEST_F(MeshLogicTest, TOFU_NewMasterAccepted_AfterStaleTimeout) {
  Mesh mesh;
  const uint8_t oldMaster[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t newMaster[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.processMasterBeacon(makeBeacon(oldMaster, 1, 1));

  // Advance past STALE_MASTER_THRESHOLD_MS (9000ms)
  advanceMillis(9001);

  mesh.processMasterBeacon(makeBeacon(newMaster, 2, 1));
  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, newMaster, 6), 0);
}

// --- Beacon relay dedup ---

TEST_F(MeshLogicTest, BeaconRelay_SameEpochSeq_SuppressedRelay) {
  Mesh mesh;
  // Set as non-master
  mesh.isMaster = false;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

  // First beacon — sets relay pending
  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 5));
  EXPECT_TRUE(mesh.relayPending);

  // Drain relay (simulate loop)
  mesh.relayPending = false;

  size_t sendsBefore = espNowSentPackets.size();
  // Same beacon arrives again (duplicate path, e.g. multi-hop echo)
  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 5));
  // Relay should NOT fire — same epoch+seq
  EXPECT_FALSE(mesh.relayPending);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
}

TEST_F(MeshLogicTest, BeaconRelay_NewerSeq_AllowsRelay) {
  Mesh mesh;
  mesh.isMaster = false;
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 5));
  mesh.relayPending = false; // Drain

  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 6)); // Newer seq
  EXPECT_TRUE(mesh.relayPending);
}

// --- Dual master mode ---

TEST_F(MeshLogicTest, DualMaster_SecondBeaconFromNewMAC_LearnedAsSecondary) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  // Learn primary via first beacon
  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));
  ASSERT_TRUE(mesh.enrollment.hasMasterMac);

  // Second beacon from different MAC — must be learned as secondary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1));

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

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Beacon from primary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.processMasterBeacon(makeBeacon(primaryMac, 2, 1));

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known primary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_BeaconFromSecondaryMAC_Accepted) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Beacon from secondary — must not be rejected and relayPending must fire
  mesh.isMaster = false;
  mesh.relayPending = false;
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 2, 1));

  EXPECT_TRUE(mesh.relayPending) << "Beacon from known secondary must set relayPending";
}

TEST_F(MeshLogicTest, DualMaster_ImpostorMAC_Rejected_WhenBothMastersKnown) {
  Mesh mesh;
  mesh.setDualMasterMode(true);
  const uint8_t primaryMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t secondaryMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t impostorMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

  mesh.processMasterBeacon(makeBeacon(primaryMac, 1, 1));   // learn primary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1)); // learn secondary

  // Third distinct MAC while both masters fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.processMasterBeacon(makeBeacon(impostorMac, 1, 2));

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

  mesh.processMasterBeacon(makeBeacon(knownMac, 1, 1));

  // Second distinct MAC while single master still fresh — must be rejected
  size_t sendsBefore = espNowSentPackets.size();
  mesh.processMasterBeacon(makeBeacon(unknownMac, 1, 2));

  EXPECT_EQ(memcmp(mesh.enrollment.knownMasterMac, knownMac, 6), 0)
      << "Known master MAC must not change";
  EXPECT_FALSE(mesh.enrollment.hasMasterMacSecondary);
  EXPECT_EQ(espNowSentPackets.size(), sendsBefore);
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
  p1.lastSeenMillis = 0;
  mesh.peers.append(p1);
  PeerInfo p2{};
  memcpy(p2.mac, kPeer2Mac, 6);
  p2.lastSeenMillis = 0;
  mesh.peers.append(p2);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1, /*hopCount=*/1);

  mesh.relayDownlink(msg);

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
  p1.lastSeenMillis = 0;
  mesh.peers.append(p1);

  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 1,
                         /*hopCount=*/lattice::config::MAX_HOPS);

  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, SkipsSelf_WhenSelfInPeerList) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMillis = 0;
  mesh.peers.append(p1);
  // Add self to peer list (shouldn't happen in production but guard against it)
  PeerInfo self{};
  memcpy(self.mac, kMyMac, 6);
  self.lastSeenMillis = 0;
  mesh.peers.append(self);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1);
  mesh.relayDownlink(msg);

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
    p.lastSeenMillis = 0;
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
  sensorPeer.lastSeenMillis = 0;
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
  extra.lastSeenMillis = 0;
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
  extra.lastSeenMillis = 0;
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
  extra.lastSeenMillis = 0;
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
    p.lastSeenMillis = 0;
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
    leaf.lastSeenMillis = 0;
    mesh.peers.append(leaf);
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

  // Real Curve25519 keypairs: ESP-NOW peer registration no longer derives a
  // per-peer LMK (Task 8 — link layer is unencrypted; E2E AEAD is the security
  // boundary), but the stored public key still feeds E2E key derivation
  // elsewhere, so keep using real (non-zeroed) keys here.
  uint8_t nodePriv[32], nodePub[32], masterPriv[32], masterKey[32];
  lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
  lattice::mesh::crypto::generateKeypair(masterPriv, masterKey);
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
  lattice::mesh::crypto::registerPeerWithEspNow(kPeerMac);

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
  master.testRoutes().record(leaf, path, 2, 1000);

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
  lattice::mesh::crypto::registerPeerWithEspNow(kPeerMac);

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
  EXPECT_EQ(0, memcmp(ack.secondary_master_mac, sec, 6));
  EXPECT_EQ(0, memcmp(ack.secondary_public_key, secPub, 32));
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
  EXPECT_EQ(0, memcmp(ack.secondary_master_mac, zero6, 6));
  EXPECT_EQ(0, memcmp(ack.secondary_public_key, zero32, 32));
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
    master.lastSeenMillis = 0;
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

class JoinAckForgeryTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
    lattice::mesh::crypto::generateKeypair(nodePriv, nodePub);
    lattice::mesh::crypto::generateKeypair(masterPriv, masterKey);
    lattice::mesh::crypto::generateKeypair(attackerPriv, attackerKey);
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
    master.lastSeenMillis = 0;
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

// ─── drainRecvQueue: replay protection ───────────────────────────────────────
// Relay dedup is drainRecvQueue's responsibility; relay paths no longer call
// isReplay directly. These tests verify that the production path (drainRecvQueue
// → dispatch) correctly drops replayed messages before handlers are invoked.

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
    Mesh::RecvQueueEntry& slot = mesh.recvQueue[mesh.recvQueueHead];
    memcpy(&slot.msg, &msg, sizeof(msg));
    memcpy(slot.srcMac, msg.origin_mac_address, 6);
    mesh.recvQueueHead = (mesh.recvQueueHead + 1) % Mesh::RECV_QUEUE_SIZE;
    mesh.drainRecvQueue();
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
  origin.lastSeenMillis = 0;
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

  injectAndDrain(mesh, msg);    // replay: drainRecvQueue drops before dispatch
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
  origin.lastSeenMillis = 0;
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
  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 1u);
  injectAndDrain(mesh, msg); // duplicate (same epoch/seq): dropped before processRequest
  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 1u) << "duplicate must not enqueue a second relay";
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
  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 2u) << "retry must still be forwarded";
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
  mesh.replay.init(5); // bootEpoch = 5 (> 0, so the drainRecvQueue replay gate applies)

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

  ASSERT_EQ(mesh.enrollment._pendingRelayCount, 1u);
  const auto& e = mesh.enrollment._pendingRelayQueue[mesh.enrollment._pendingRelayHead];
  EXPECT_EQ(memcmp(e.mac, kMac, 6), 0);
  EXPECT_EQ(memcmp(e.pubKey, kKey, 32), 0)
      << "Full 32-byte key must be copied without chunk reassembly";
}

// ---- EnrollmentRelayCallbackTest ----

static const uint8_t* g_capturedMac = nullptr;
static const uint8_t* g_capturedKey = nullptr;

static void captureRelayFn(const uint8_t mac[6], const uint8_t pubKey[32]) {
  g_capturedMac = mac;
  g_capturedKey = pubKey;
}

class EnrollmentRelayCallbackTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_capturedMac = nullptr;
    g_capturedKey = nullptr;
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

  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 0u) << "queue must be empty after drain";
  ASSERT_NE(g_capturedMac, nullptr) << "callback was not called";
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

  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 0u) << "queue must clear even with no callback";
  EXPECT_EQ(g_capturedMac, nullptr) << "callback must not fire when unregistered";
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
  ASSERT_EQ(mesh.enrollment._pendingRelayCount, 2u);

  mesh.enrollment.drainPendingRelay();

  ASSERT_EQ(drained.size(), 2u) << "both queued relays must fire (Bug #6 starvation)";
  EXPECT_EQ(memcmp(drained[0].data(), kMacA, 6), 0) << "FIFO order: A first";
  EXPECT_EQ(memcmp(drained[1].data(), kMacB, 6), 0) << "FIFO order: B second";
  EXPECT_EQ(mesh.enrollment._pendingRelayCount, 0u);
}
