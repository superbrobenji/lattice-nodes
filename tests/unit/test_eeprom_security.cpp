#include <gtest/gtest.h>
#include <cstring>
#include "time_mock.h"
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromSecurity.h"
#include <nvs.h>
#include "error/Error.h"

// -----------------------------------------------------------------------
// Test fixture
//
// lattice::eeprom holds its state in file-static storage (Phase H2 item AA:
// migrated off the old Meyers-singleton EepromManager class) — isInitialized
// stays true for the process lifetime. Between tests we clear the
// NvsMock static store (tests/mocks/nvs.h, Phase I Task 4) to get a clean
// slate.
// -----------------------------------------------------------------------

class EepromSecurityTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    // Clear the NVS mock's static store
    NvsMock::_store.clear();
    // Init the singleton (idempotent — no-op after first call)
    namespace mgr = lattice::eeprom;
    mgr::init();
    // Reset devMode to false (singleton persists across tests)
    mgr::setDevMode(false);
  }
};

// -----------------------------------------------------------------------
// Mesh key
// -----------------------------------------------------------------------

TEST_F(EepromSecurityTest, MeshKey_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;

  uint8_t key[16];
  for (int i = 0; i < 16; ++i) {
    key[i] = static_cast<uint8_t>(i + 100);
  }

  mgr::saveMeshKey(key, 16);

  uint8_t loaded[16]{};
  EXPECT_TRUE(mgr::loadMeshKey(loaded, 16));
  EXPECT_EQ(memcmp(loaded, key, 16), 0);
}

TEST_F(EepromSecurityTest, MeshKey_Load_NotFound_ReturnsFalse) {
  uint8_t key[16]{};
  EXPECT_FALSE(lattice::eeprom::loadMeshKey(key, 16));
}

TEST_F(EepromSecurityTest, MeshKey_WrongSize_ReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t key[16]{};
  EXPECT_FALSE(mgr::loadMeshKey(key, 8)); // Wrong size
}

// -----------------------------------------------------------------------
// Known master MAC
// -----------------------------------------------------------------------

TEST_F(EepromSecurityTest, KnownMasterMac_UnsetReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t mac[6] = {};
  bool found = mgr::loadKnownMasterMac(mac);
  EXPECT_FALSE(found);
}

TEST_F(EepromSecurityTest, KnownMasterMac_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr::saveKnownMasterMac(expected);

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMac(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EepromSecurityTest, KnownMasterMac_Clear_ResetsToUnset) {
  namespace mgr = lattice::eeprom;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr::saveKnownMasterMac(mac);
  mgr::clearKnownMasterMac();

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMac(loaded);
  EXPECT_FALSE(found);
}

TEST_F(EepromSecurityTest, KnownMasterMac_AllFF_TreatedAsUnset) {
  namespace mgr = lattice::eeprom;
  const uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  mgr::saveKnownMasterMac(mac);

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMac(loaded);
  EXPECT_FALSE(found);
}

// -----------------------------------------------------------------------
// Known master MAC secondary
// -----------------------------------------------------------------------

TEST_F(EepromSecurityTest, KnownMasterMacSecondary_UnsetReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t mac[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(mac);
  EXPECT_FALSE(found);
}

TEST_F(EepromSecurityTest, KnownMasterMacSecondary_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr::saveKnownMasterMacSecondary(expected);

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EepromSecurityTest, KnownMasterMacSecondary_Clear_ResetsToUnset) {
  namespace mgr = lattice::eeprom;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr::saveKnownMasterMacSecondary(mac);
  mgr::clearKnownMasterMacSecondary();

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(loaded);
  EXPECT_FALSE(found);
}

// -----------------------------------------------------------------------
// Tiered NVS write-return escalation (issue #43)
// -----------------------------------------------------------------------

TEST_F(EepromSecurityTest, SaveKnownMasterMac_ShortWrite_Fatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  NvsMock::_failNextWriteKey = lattice::utils::NVS_KEYS::KNOWN_MASTER_MAC;
  EXPECT_THROW(mgr::saveKnownMasterMac(mac), lattice::err::FatalError);
  NvsMock::_failNextWriteKey = nullptr;
}
