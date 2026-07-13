#include <gtest/gtest.h>
#include "Mesh/Mesh.h"
#include "esp_now_mock.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "lib/lattice-protocol/c/opcodes.h"

using namespace lattice::mesh;

class RouteReportTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    resetEspNowMock();
  }

  // Set up mesh as non-master with a reachable master peer
  void setupRelayNode(Mesh& mesh, const uint8_t masterMac[6]) {
    static const uint8_t myMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    memcpy(mesh.deviceMacAddress, myMac, 6);
    mesh.isMaster = false;
    mesh.hasMasterMac = true;
    memcpy(mesh.knownMasterMac, masterMac, 6);
    memcpy(mesh.currentMaster.mac, masterMac, 6);
    mesh.currentMaster.distance = 1;
    memcpy(mesh.currentMaster.nextHop, masterMac, 6);

    // Register master as a live peer so findNextHopToMaster() returns it
    PeerInfo peer{};
    memcpy(peer.mac, masterMac, 6);
    peer.lastSeenMillis = millis();
    mesh.appendPeer(peer);
  }

  mesh_message lastSentMsg() {
    if (espNowSentPackets.empty()) {
      ADD_FAILURE() << "espNowSentPackets is empty — expected at least one sent packet";
      return {};
    }
    return *reinterpret_cast<const mesh_message*>(espNowSentPackets.back().data.data());
  }
};

TEST_F(RouteReportTest, SendRouteReport_MessageType) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();

  EXPECT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
}

TEST_F(RouteReportTest, SendRouteReport_PayloadStructure) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh.sendRouteReport();

  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.data[0], OP_ROUTE_REPORT);  // 0xB3
  EXPECT_EQ(sent.data[1], 0);                // path_len = 0 on origin
  // data[2..63] should be zeroed
  for (int i = 2; i < 64; ++i) {
    EXPECT_EQ(sent.data[i], 0) << "data[" << i << "] should be 0";
  }
}

TEST_F(RouteReportTest, SendRouteReport_NotSentByMaster) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);
  mesh.isMaster = true;

  size_t before = espNowSentPackets.size();
  mesh.sendRouteReport();  // master should be a no-op

  EXPECT_EQ(espNowSentPackets.size(), before);
}
