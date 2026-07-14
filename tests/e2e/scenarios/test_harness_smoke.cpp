#include <gtest/gtest.h>
#include "harness/NodeContext.h"
#include "harness/SimClock.h"
#include "harness/SimNode.h"
#include "harness/SimWorld.h"
#include "EEPROM.h"
#include "esp_wifi_mock.h"
#include "src/persistence/EepromManager.h"
#include "src/mesh/Mesh.h"
#include "lib/lattice-protocol/c/message_types.h"
#include "lib/lattice-protocol/c/mesh_message.h"

TEST(NodeContextSwap, IsolatesEepromAndMac) {
  sim::NodeContext a, b;
  a.mac[0] = 0xAA;
  b.mac[0] = 0xBB;

  sim::swapIn(a);
  EEPROM.write(0, 0x11);
  EXPECT_EQ(mockDeviceMac[0], 0xAA);
  sim::swapOut(a);

  sim::swapIn(b);
  EXPECT_EQ(EEPROM.read(0), 0xFF) << "node B must not see node A's EEPROM";
  EXPECT_EQ(mockDeviceMac[0], 0xBB);
  EEPROM.write(0, 0x22);
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_EQ(EEPROM.read(0), 0x11);
  sim::swapOut(a);
}

TEST(NodeContextSwap, FreshContextGetsPristineSingletons) {
  sim::NodeContext a, b;

  sim::swapIn(a);
  EXPECT_FALSE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "sanity: singleton must start uninitialized before node A dirties it";
  lattice::utils::EepromManager::getInstance().init(); // node A dirties singleton state
  EXPECT_TRUE(lattice::utils::EepromManager::getInstance().isInitializedForTest());
  sim::swapOut(a);

  sim::swapIn(b); // fresh context: must NOT inherit A's initialized state
  EXPECT_FALSE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "node B's fresh context must restore pristine EepromManager state, "
      << "not silently inherit node A's initialized singleton";
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_TRUE(lattice::utils::EepromManager::getInstance().isInitializedForTest())
      << "swapping back to A must still see A's own initialized state";
  sim::swapOut(a);
}

TEST(SimClockTest, AdvancesMillis) {
  sim::SimClock clock;
  uint32_t t0 = clock.now();
  clock.advance(250);
  EXPECT_EQ(clock.now(), t0 + 250);
  EXPECT_EQ(millis(), clock.now());
}

static sim::NodeConfig masterCfg() {
  return {{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER};
}

TEST(SimNodeTest, MasterBootsAndBeacons) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  // Master beacon interval is 3000ms — tick across 4s of virtual time
  for (int i = 0; i < 4000; ++i) {
    clock.advance(1);
    master.tick();
  }
  // Beacon(s) must have been captured as broadcast esp_now sends
  bool sawBeacon = false;
  for (const auto& pkt : master.ctx().espNowSent) {
    if (pkt.data.size() == sizeof(mesh_message)) {
      const auto* msg = reinterpret_cast<const mesh_message*>(pkt.data.data());
      if (msg->message_type == MESH_TYPE_MASTER_BEACON) sawBeacon = true;
    }
  }
  EXPECT_TRUE(sawBeacon);
}

TEST(SimNodeTest, RebootPreservesEeprom) {
  sim::SimClock clock;
  sim::SimNode master(masterCfg());
  master.boot();
  auto imageBefore = master.ctx().eepromData;
  master.reboot();
  // Master flag survives; keypair survives (same EEPROM bytes at PRIVATE_KEY range)
  EXPECT_TRUE(std::equal(imageBefore.begin() + 417, imageBefore.begin() + 483,
                         master.ctx().eepromData.begin() + 417));
  EXPECT_EQ(master.ctx().eepromData[0], imageBefore[0]);
}

TEST(VirtualBusTest, BeaconReachesLinkedNodeOnly) {
  sim::SimWorld world;
  auto* master = world.addNode({{0x02, 0, 0, 0, 0, 0x01}, true, lattice::adapter::SERIAL_ADAPTER});
  auto* near = world.addNode({{0x02, 0, 0, 0, 0, 0x02}, false, lattice::adapter::PIR_ADAPTER});
  auto* far = world.addNode({{0x02, 0, 0, 0, 0, 0x03}, false, lattice::adapter::PIR_ADAPTER});
  world.bus.link(master, near);
  // 'far' deliberately unlinked

  world.run(4000); // > one beacon interval

  // Linked node has processed a beacon: its mesh learned the master MAC
  bool nearKnowsMaster = near->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  bool farKnowsMaster = far->with([&](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) {
    return memcmp(m.currentMaster.mac, master->mac(), 6) == 0;
  });
  EXPECT_TRUE(nearKnowsMaster);
  EXPECT_FALSE(farKnowsMaster);
}
