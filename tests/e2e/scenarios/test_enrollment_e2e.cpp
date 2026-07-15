// First full-stack scenario coverage built on the shared MeshSimTest fixture
// (harness/MeshSimTest.h). Exercises the complete enrollment chain already
// proven piecewise in Tasks 6/6a (test_harness_smoke.cpp): unenrolled node's
// periodic ENROLLMENT broadcast -> master's relay-to-serial -> hub sees it ->
// hub approves -> master's Mesh::enrollPeer -> mesh-side JOIN_ACK back to the
// node -> node's Enrollment::processJoinAck saves the enrolled flag to
// EEPROM.
#include "harness/MeshSimTest.h"

TEST_F(MeshSimTest, NodeEnrollsThroughMasterAndHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);

  EXPECT_FALSE(sensor->isEnrolled());
  enroll(sensor); // asserts request reached hub + JOIN_ACK enrolled the node
}

TEST_F(MeshSimTest, UnenrolledNodeDataNeverReachesHub) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);

  // No JOIN_ACK issued. PIR data must not appear at the hub.
  runPolled(11000);
  // Node is un-enrolled: its tick() returns before adapter->loop(); motion can't send.
  EXPECT_FALSE(sensor->isEnrolled());
  EXPECT_TRUE(hub->adapterDataFromOrigin(sensor->mac()).empty());
}

TEST_F(MeshSimTest, EnrollmentSurvivesReboot) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  sensor->reboot();
  EXPECT_TRUE(sensor->isEnrolled()) << "enrolled flag must persist in EEPROM";
}
