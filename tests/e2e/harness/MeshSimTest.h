#pragma once
// Shared fixture for full-stack mesh e2e scenarios: owns a SimWorld + FakeHub,
// provides master/sensor node factories, a polling helper (so partial serial
// frames assemble across virtual time), and a happy-path enroll() helper that
// drives a node all the way from its first ENROLLMENT broadcast through the
// hub's approval to a saved EEPROM-enrolled flag (see FakeHubTest and
// ServerBroadcastTest in test_harness_smoke.cpp for the original inline
// version this was lifted from).
#include <gtest/gtest.h>
#include "harness/SimWorld.h"
#include "harness/FakeHub.h"
#include "harness/MasterKeypairFixture.h"
#include "src/error/Error.h"
#include "config/master_pubkey_pin_wrapper.h"

class MeshSimTest : public ::testing::Test {
protected:
  sim::SimWorld world;
  std::unique_ptr<sim::FakeHub> hub;
  sim::SimNode* master = nullptr;

  // Phase D (#42): equals lattice::mesh::pin::MASTER_MAC exactly, and addMaster()
  // below seeds this node with sim::fixture::MASTER_PRIVATE/PUBLIC_KEY (which
  // equals lattice::mesh::pin::MASTER_PUBKEY) — so this fixture's master stays in
  // the pin-active regime rather than being dropped by the JOIN_ACK/beacon pin
  // checks (Enrollment::processJoinAck / Mesh::processMasterBeacon).
  static constexpr uint8_t MAC_MASTER[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  static constexpr uint8_t MAC_NODE_A[6] = {0x02, 0, 0, 0, 0, 0x0A};
  static constexpr uint8_t MAC_NODE_B[6] = {0x02, 0, 0, 0, 0, 0x0B};

  void SetUp() override {
    lattice_test_errFailCount = 0;
    _mockMillis = 0;
    // Phase D (#42): pin-active by default so e2e scenarios exercise the same
    // production check unit tests do. DualMasterTest (test_dual_master_e2e.cpp)
    // overrides this to bypass — a single MASTER_MAC pin cannot simultaneously
    // match both a primary and a secondary master's real MAC, so seeding the
    // pin for that fixture is infeasible (design doc §5/§7).
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
    EXPECT_EQ(lattice_test_errFailCount, 0) << "a node hit err::fail during the scenario";
  }

  sim::SimNode* addMaster() {
    sim::NodeConfig cfg{};
    memcpy(cfg.mac, MAC_MASTER, 6);
    cfg.isMaster = true;
    cfg.adapterType = lattice::adapter::SERIAL_ADAPTER;
    cfg.seedPrivateKey32 = sim::fixture::MASTER_PRIVATE_KEY;
    cfg.seedPublicKey32 = sim::fixture::MASTER_PUBLIC_KEY;
    master = world.addNode(cfg);
    hub = std::make_unique<sim::FakeHub>(master);
    return master;
  }
  sim::SimNode* addSensor(const uint8_t mac[6]) {
    sim::NodeConfig cfg{};
    memcpy(cfg.mac, mac, 6);
    cfg.isMaster = false;
    cfg.adapterType = lattice::adapter::PIR_ADAPTER;
    return world.addNode(cfg);
  }
  // Run world + poll hub every virtual 100ms so partial frames assemble
  void runPolled(uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 100) {
      world.run(std::min<uint32_t>(100, ms - done));
      if (hub)
        hub->poll();
    }
  }
  // Full happy-path enrollment of `node`; asserts success
  void enroll(sim::SimNode* node) {
    runPolled(11000);
    const mesh_message* req = hub->enrollmentFrom(node->mac());
    ASSERT_NE(req, nullptr) << "enrollment request never reached hub";
    hub->approveEnrollment(node->mac(), req->enrollment_public_key);
    runPolled(5000);
    ASSERT_TRUE(node->isEnrolled());
  }
};
