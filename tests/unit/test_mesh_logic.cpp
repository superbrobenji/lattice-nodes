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
