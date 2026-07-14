#include <gtest/gtest.h>
#include "mesh/Mesh.h"
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
    mesh.peers.append(peer);
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

TEST_F(RouteReportTest, ProcessRouteReport_RelayAppendsMAC) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  const uint8_t originMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  const uint8_t myMac[6]     = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);
  memcpy(mesh.deviceMacAddress, myMac, 6); // set after setupRelayNode (which overwrites it)

  // Build a route report with path_len=1 (one relay MAC already written)
  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data_type = 0;
  msg.hop_count = 1;
  memcpy(msg.origin_mac_address, originMac, 6);
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 1; // one relay already written
  // data[2..7] = some relay MAC
  memset(&msg.data[2], 0xAB, 6);

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  ASSERT_EQ(espNowSentPackets.size(), before + 1);
  mesh_message sent = lastSentMsg();
  EXPECT_EQ(sent.message_type, MESH_TYPE_ROUTE_REPORT);
  EXPECT_EQ(sent.data[1], 2); // path_len incremented to 2

  // Our device MAC should be at data[8..13] (index 1 = second relay slot)
  EXPECT_EQ(memcmp(&sent.data[8], myMac, 6), 0);
  EXPECT_EQ(sent.hop_count, 2);
}

TEST_F(RouteReportTest, ProcessRouteReport_MasterDeliversToCallback) {
  Mesh mesh;
  mesh.isMaster = true;

  bool callbackFired = false;
  mesh_message received{};
  mesh.linkDataRecvCallback([&](const mesh_message& m) {
    callbackFired = true;
    received = m;
  });

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 1;

  mesh.processRouteReport(msg);

  EXPECT_TRUE(callbackFired);
  EXPECT_EQ(received.data[0], OP_ROUTE_REPORT);
}

TEST_F(RouteReportTest, ProcessRouteReport_PathFullDropsMessage) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 10; // path full — max 10 relay MACs

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no message sent
}

TEST_F(RouteReportTest, ProcessRouteReport_MalformedOpcodeDropsMessage) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = 0xFF; // wrong opcode
  msg.data[1] = 0;

  size_t before = espNowSentPackets.size();
  mesh.processRouteReport(msg);

  EXPECT_EQ(espNowSentPackets.size(), before); // no message sent
}

TEST_F(RouteReportTest, DrainRecvQueue_DispatchesRouteReport) {
  const uint8_t masterMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  Mesh mesh;
  setupRelayNode(mesh, masterMac);

  mesh_message msg{};
  msg.proto_version = PROTO_VERSION;
  msg.message_type = MESH_TYPE_ROUTE_REPORT;
  msg.data[0] = OP_ROUTE_REPORT;
  msg.data[1] = 0;

  // Directly push to recv queue (UNIT_TEST exposes all members)
  uint8_t srcMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t nextHead = (mesh.recvQueueHead + 1) % Mesh::RECV_QUEUE_SIZE;
  memcpy(mesh.recvQueue[mesh.recvQueueHead].srcMac, srcMac, 6);
  mesh.recvQueue[mesh.recvQueueHead].msg = msg;
  mesh.recvQueueHead = nextHead;

  size_t before = espNowSentPackets.size();
  mesh.drainRecvQueue();

  EXPECT_GT(espNowSentPackets.size(), before); // relayed the message
}
