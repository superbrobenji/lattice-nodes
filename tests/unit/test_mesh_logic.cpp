#include <gtest/gtest.h>
#include "Mesh/Mesh.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"

using namespace planetopia::mesh;

class MeshLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  mesh_message makeBeacon(const uint8_t masterMac[6], uint32_t epoch, uint16_t seq) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType = MESH_TYPE_MASTER_BEACON;
    m.epochNum = epoch;
    m.seqNum = seq;
    memcpy(m.originMacAddress, masterMac, 6);
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

  EXPECT_TRUE(mesh.hasMasterMac);
  EXPECT_EQ(memcmp(mesh.knownMasterMac, masterMac, 6), 0);
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
  EXPECT_TRUE(mesh.hasMasterMac);
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
  EXPECT_EQ(memcmp(mesh.knownMasterMac, realMaster, 6), 0);
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
  EXPECT_EQ(memcmp(mesh.knownMasterMac, newMaster, 6), 0);
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
  ASSERT_TRUE(mesh.hasMasterMac);

  // Second beacon from different MAC — must be learned as secondary
  mesh.processMasterBeacon(makeBeacon(secondaryMac, 1, 1));

  EXPECT_TRUE(mesh.hasMasterMacSecondary);
  EXPECT_EQ(memcmp(mesh.knownMasterMacSecondary, secondaryMac, 6), 0);
  // Primary must still be unchanged
  EXPECT_EQ(memcmp(mesh.knownMasterMac, primaryMac, 6), 0);
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
  EXPECT_EQ(memcmp(mesh.knownMasterMac, primaryMac, 6), 0);
  EXPECT_EQ(memcmp(mesh.knownMasterMacSecondary, secondaryMac, 6), 0);
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

  EXPECT_EQ(memcmp(mesh.knownMasterMac, knownMac, 6), 0) << "Known master MAC must not change";
  EXPECT_FALSE(mesh.hasMasterMacSecondary);
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
    m.protoVersion = 1;
    m.messageType = MESH_TYPE_ADAPTER_DATA;
    m.dataType = adapter_types::PIR_ADAPTER;
    memcpy(m.originMacAddress, origin, 6);
    memcpy(m.targetMacAddress, target, 6);
    memcpy(m.lastHopMacAddress, origin, 6);
    m.hopCount = hopCount;
    m.epochNum = epoch;
    m.seqNum = seq;
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
  mesh.appendPeer(p1);
  PeerInfo p2{};
  memcpy(p2.mac, kPeer2Mac, 6);
  p2.lastSeenMillis = 0;
  mesh.appendPeer(p2);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1, /*hopCount=*/1);

  mesh.relayDownlink(msg);

  // 2 peers → 2 sends
  EXPECT_EQ(espNowSentPackets.size(), 2u);
  for (const auto& pkt : espNowSentPackets) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(pkt.data.data());
    EXPECT_EQ(sent.hopCount, 2u);                              // incremented
    EXPECT_EQ(memcmp(sent.targetMacAddress, kPeer2Mac, 6), 0); // target preserved
    EXPECT_EQ(memcmp(sent.lastHopMacAddress, kMyMac, 6), 0);   // lastHop = my MAC
  }
}

TEST_F(RelayDownlinkTest, DropsAtMaxHops) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMillis = 0;
  mesh.appendPeer(p1);

  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 1,
                         /*hopCount=*/planetopia::config::MAX_HOPS);

  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, SkipsSelf_WhenSelfInPeerList) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{};
  memcpy(p1.mac, kPeer1Mac, 6);
  p1.lastSeenMillis = 0;
  mesh.appendPeer(p1);
  // Add self to peer list (shouldn't happen in production but guard against it)
  PeerInfo self{};
  memcpy(self.mac, kMyMac, 6);
  self.lastSeenMillis = 0;
  mesh.appendPeer(self);

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
    memcpy(mesh.currentMaster.nextHop, kMasterMac, 6);
    mesh.currentMaster.distance = 1;
    mesh.hasMasterMac = true;
    memcpy(mesh.knownMasterMac, kMasterMac, 6);
    // Register master as enrolled peer (required for sendMessage + isPeerInRange)
    PeerInfo p{};
    memcpy(p.mac, kMasterMac, 6);
    p.lastSeenMillis = 0;
    mesh.appendPeer(p);
    return mesh;
  }

  mesh_message makeUplinkMsg(uint32_t epoch, uint16_t seq, uint8_t hopCount = 1) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType = MESH_TYPE_ADAPTER_DATA;
    m.dataType = adapter_types::PIR_ADAPTER;
    memcpy(m.originMacAddress, kSensorMac, 6);
    memcpy(m.targetMacAddress, kMasterMac, 6); // addressed to master
    memcpy(m.lastHopMacAddress, kSensorMac, 6);
    m.hopCount = hopCount;
    m.epochNum = epoch;
    m.seqNum = seq;
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
  EXPECT_EQ(sent.hopCount, 2u);                                       // incremented
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kMasterMac, 6), 0); // routed via nextHop
}

TEST_F(AdapterDataRelayTest, Master_DoesNotRelayUplink_DeliversLocally) {
  Mesh mesh = makeIntermediateNode();
  mesh.isMaster = true;

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](mesh_message) { callbackFired = true; });

  auto msg = makeUplinkMsg(1, 1);
  memcpy(msg.targetMacAddress, mesh.deviceMacAddress, 6); // addressed to self (master)

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
  mesh.appendPeer(extra);

  mesh_message msg{};
  msg.protoVersion = 1;
  msg.messageType = MESH_TYPE_ADAPTER_DATA;
  msg.dataType = adapter_types::PIR_ADAPTER;
  memcpy(msg.originMacAddress, kMasterMac, 6);
  memcpy(msg.targetMacAddress, kSensorMac, 6); // some other sensor, not me, not master
  msg.hopCount = 1;
  msg.epochNum = 2;
  msg.seqNum = 1;

  size_t before = espNowSentPackets.size();
  mesh.processAdapterData(msg);

  // Should relay to all peers: kMasterMac + kPeerMac (2 peers)
  EXPECT_GT(espNowSentPackets.size(), before);
  // Target preserved in every relayed copy
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.targetMacAddress, kSensorMac, 6), 0);
    EXPECT_EQ(sent.hopCount, 2u);
  }
}

TEST_F(AdapterDataRelayTest, IntermediateNode_BroadcastTarget_DeliveredAndRelayed) {
  Mesh mesh = makeIntermediateNode();
  PeerInfo extra{};
  memcpy(extra.mac, kPeerMac, 6);
  extra.lastSeenMillis = 0;
  mesh.appendPeer(extra);

  bool callbackFired = false;
  mesh.linkDataRecvCallback([&](mesh_message) { callbackFired = true; });

  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  mesh_message msg{};
  msg.protoVersion = 1;
  msg.messageType = MESH_TYPE_ADAPTER_DATA;
  msg.dataType = adapter_types::PIR_ADAPTER;
  memcpy(msg.originMacAddress, kMasterMac, 6);
  memcpy(msg.targetMacAddress, kBroadcast, 6); // broadcast
  msg.hopCount = 1;
  msg.epochNum = 3;
  msg.seqNum = 1;

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
  mesh.appendPeer(extra);

  static constexpr uint8_t kPayload[64] = {0x01, 0x02, 0x03};
  size_t before = espNowSentPackets.size();
  mesh.broadcastAdapterData(adapter_types::PIR_ADAPTER, kPayload);

  EXPECT_GT(espNowSentPackets.size(), before);
  // Every sent message should have FF:FF target
  static constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  for (size_t i = before; i < espNowSentPackets.size(); ++i) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets[i].data.data());
    EXPECT_EQ(memcmp(sent.targetMacAddress, kBroadcast, 6), 0);
  }
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
    mesh.appendPeer(p);
    return mesh;
  }

  mesh_message makeJoinAck(const uint8_t target[6], uint8_t hopCount = 1) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType = MESH_TYPE_JOIN_ACK;
    m.dataType = adapter_types::UNKNOWN_ADAPTER;
    memcpy(m.originMacAddress, kMasterMac, 6);
    memcpy(m.targetMacAddress, target, 6);
    memcpy(m.lastHopMacAddress, kMasterMac, 6);
    m.hopCount = hopCount;
    m.epochNum = 1;
    m.seqNum = 1;
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

  EXPECT_GT(espNowSentPackets.size(), before); // relayed to kPeerMac
  EXPECT_EQ(memcmp(espNowSentPackets.back().addr, kPeerMac, 6), 0);
  const auto& sent = *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  EXPECT_EQ(sent.hopCount, 2u);
  EXPECT_EQ(memcmp(sent.targetMacAddress, kDistantNode, 6), 0); // target preserved
}

TEST_F(JoinAckRelayTest, DoesNotRelayJoinAck_WhenAddressedToSelf) {
  // When addressed to self: process (enroll), do NOT relay
  Mesh mesh = makeIntermediateNode();

  // Provide a fingerprint (first 4 bytes of devicePublicKey)
  // devicePublicKey is zeroed in constructor — fingerprint = {0,0,0,0}
  auto msg = makeJoinAck(kMyMac);
  memset(msg.data, 0, sizeof(msg.data)); // fingerprint matches zeroed pubkey

  size_t before = espNowSentPackets.size();
  mesh.processJoinAck(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no relay
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
    memcpy(slot.srcMac, msg.originMacAddress, 6);
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
  int deliveredCount = 0;
  mesh.linkDataRecvCallback([&](mesh_message) { ++deliveredCount; });

  mesh_message msg{};
  msg.protoVersion = 2;
  msg.messageType = MESH_TYPE_ADAPTER_DATA;
  msg.dataType = adapter_types::PIR_ADAPTER;
  memcpy(msg.originMacAddress, kOriginMac, 6);
  memcpy(msg.targetMacAddress, kMyMac, 6);
  msg.epochNum = 1;
  msg.seqNum = 42;

  injectAndDrain(mesh, msg); // first: not replay — delivered
  EXPECT_EQ(deliveredCount, 1);

  injectAndDrain(mesh, msg);    // replay: drainRecvQueue drops before dispatch
  EXPECT_EQ(deliveredCount, 1); // callback not invoked again
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
  memcpy(mesh.devicePublicKey, kPubKey, 32);

  mesh.sendEnrollmentRequest();

  ASSERT_EQ(espNowSentPackets.size(), 1u)
      << "Expected exactly 1 ESP-NOW packet (was 3 with old chunking)";
  const auto& pkt = espNowSentPackets[0];
  ASSERT_GE(pkt.data.size(), sizeof(mesh_message));
  const mesh_message* sent = reinterpret_cast<const mesh_message*>(pkt.data.data());
  EXPECT_EQ(sent->messageType, MESH_TYPE_ENROLLMENT);
  EXPECT_EQ(memcmp(sent->enrollmentPublicKey, kPubKey, 32), 0)
      << "Full public key must be present in a single message";
}

TEST_F(EnrollmentTest, ProcessSingleMessageSetsKey) {
  Mesh mesh;
  mesh.isMaster = true;

  mesh_message msg = {};
  msg.messageType = MESH_TYPE_ENROLLMENT;
  static constexpr uint8_t kMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  static constexpr uint8_t kKey[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                       0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                       0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
  memcpy(msg.originMacAddress, kMac, 6);
  memcpy(msg.enrollmentPublicKey, kKey, 32);

  mesh.processEnrollmentRequest(msg);

  EXPECT_TRUE(mesh._pendingEnrollmentRelay);
  EXPECT_EQ(memcmp(mesh._pendingEnrollmentMac, kMac, 6), 0);
  EXPECT_EQ(memcmp(mesh._pendingEnrollmentPubKey, kKey, 32), 0)
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
  static constexpr uint8_t kKey[32] = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};

  memcpy(mesh._pendingEnrollmentMac, kMac, 6);
  memcpy(mesh._pendingEnrollmentPubKey, kKey, 32);
  mesh._pendingEnrollmentRelay = true;

  mesh.drainPendingEnrollment();

  EXPECT_FALSE(mesh._pendingEnrollmentRelay) << "flag must clear after drain";
  ASSERT_NE(g_capturedMac, nullptr) << "callback was not called";
  EXPECT_EQ(memcmp(g_capturedMac, kMac, 6), 0) << "wrong MAC passed to callback";
  EXPECT_EQ(memcmp(g_capturedKey, kKey, 32), 0) << "wrong pubKey passed to callback";
}

TEST_F(EnrollmentRelayCallbackTest, DrainWithNoCallbackClearsFlag) {
  Mesh mesh;
  mesh.isMaster = true;
  // No callback registered.

  mesh._pendingEnrollmentRelay = true;
  mesh.drainPendingEnrollment();

  EXPECT_FALSE(mesh._pendingEnrollmentRelay) << "flag must clear even with no callback";
  EXPECT_EQ(g_capturedMac, nullptr) << "callback must not fire when unregistered";
}
