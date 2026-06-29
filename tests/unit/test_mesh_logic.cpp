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
    m.messageType  = MESH_TYPE_MASTER_BEACON;
    m.epochNum     = epoch;
    m.seqNum       = seq;
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
  const uint8_t realMaster[6]    = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t impostorMac[6]   = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x99};

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
  mesh.relayPending = false;  // Drain

  mesh.processMasterBeacon(makeBeacon(masterMac, 1, 6));  // Newer seq
  EXPECT_TRUE(mesh.relayPending);
}

// ─── relayDownlink ───────────────────────────────────────────────────────────

class RelayDownlinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  static constexpr uint8_t kMyMac[6]    = {0x11,0x22,0x33,0x44,0x55,0x66};
  static constexpr uint8_t kPeer1Mac[6] = {0xAA,0xAA,0xAA,0xAA,0xAA,0x01};
  static constexpr uint8_t kPeer2Mac[6] = {0xBB,0xBB,0xBB,0xBB,0xBB,0x02};
  static constexpr uint8_t kOriginMac[6]= {0xCC,0xCC,0xCC,0xCC,0xCC,0x03};

  Mesh makeMeshWithPeers(int numPeers) {
    Mesh mesh;
    memcpy(mesh.deviceMacAddress, kMyMac, 6);
    if (numPeers >= 1) {
      PeerInfo p{}; memcpy(p.mac, kPeer1Mac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    }
    if (numPeers >= 2) {
      PeerInfo p{}; memcpy(p.mac, kPeer2Mac, 6); p.lastSeenMillis = 0; mesh.appendPeer(p);
    }
    return mesh;
  }

  mesh_message makeDataMsg(const uint8_t origin[6], const uint8_t target[6],
                           uint32_t epoch, uint16_t seq, uint8_t hopCount = 0) {
    mesh_message m{};
    m.protoVersion = 1;
    m.messageType  = MESH_TYPE_ADAPTER_DATA;
    m.dataType     = adapter_types::PIR_ADAPTER;
    memcpy(m.originMacAddress, origin, 6);
    memcpy(m.targetMacAddress, target, 6);
    memcpy(m.lastHopMacAddress, origin, 6);
    m.hopCount = hopCount;
    m.epochNum = epoch;
    m.seqNum   = seq;
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
  PeerInfo p1{}; memcpy(p1.mac, kPeer1Mac, 6); p1.lastSeenMillis = 0; mesh.appendPeer(p1);
  PeerInfo p2{}; memcpy(p2.mac, kPeer2Mac, 6); p2.lastSeenMillis = 0; mesh.appendPeer(p2);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1, /*hopCount=*/1);

  mesh.relayDownlink(msg);

  // 2 peers → 2 sends
  EXPECT_EQ(espNowSentPackets.size(), 2u);
  for (const auto& pkt : espNowSentPackets) {
    const auto& sent = *reinterpret_cast<const mesh_message*>(pkt.data.data());
    EXPECT_EQ(sent.hopCount, 2u);                          // incremented
    EXPECT_EQ(memcmp(sent.targetMacAddress, kPeer2Mac, 6), 0); // target preserved
    EXPECT_EQ(memcmp(sent.lastHopMacAddress, kMyMac, 6), 0);   // lastHop = my MAC
  }
}

TEST_F(RelayDownlinkTest, DropsReplay) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{}; memcpy(p1.mac, kPeer1Mac, 6); p1.lastSeenMillis = 0; mesh.appendPeer(p1);

  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 99);

  mesh.isReplay(msg);  // Pre-record in replay cache
  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, DropsAtMaxHops) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{}; memcpy(p1.mac, kPeer1Mac, 6); p1.lastSeenMillis = 0; mesh.appendPeer(p1);

  auto msg = makeDataMsg(kOriginMac, kPeer1Mac, 1, 1,
                         /*hopCount=*/planetopia::config::MAX_HOPS);

  mesh.relayDownlink(msg);

  EXPECT_EQ(espNowSentPackets.size(), 0u);
}

TEST_F(RelayDownlinkTest, SkipsSelf_WhenSelfInPeerList) {
  Mesh mesh;
  memcpy(mesh.deviceMacAddress, kMyMac, 6);
  PeerInfo p1{}; memcpy(p1.mac, kPeer1Mac, 6); p1.lastSeenMillis = 0; mesh.appendPeer(p1);
  // Add self to peer list (shouldn't happen in production but guard against it)
  PeerInfo self{}; memcpy(self.mac, kMyMac, 6); self.lastSeenMillis = 0;
  mesh.appendPeer(self);

  auto msg = makeDataMsg(kOriginMac, kPeer2Mac, 1, 1);
  mesh.relayDownlink(msg);

  // Only 1 peer (kPeer1Mac) — self skipped
  EXPECT_EQ(espNowSentPackets.size(), 1u);
  EXPECT_EQ(memcmp(espNowSentPackets[0].addr, kPeer1Mac, 6), 0);
}
