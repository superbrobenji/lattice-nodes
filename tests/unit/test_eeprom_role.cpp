#include <gtest/gtest.h>
#include <cstring>
#include "time_mock.h"
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromRole.h"
#include <nvs.h>

// -----------------------------------------------------------------------
// Test fixture
//
// lattice::eeprom holds its state in file-static storage (Phase H2 item AA:
// migrated off the old Meyers-singleton EepromManager class) — isInitialized
// stays true for the process lifetime. Between tests we clear the
// NvsMock static store (tests/mocks/nvs.h, Phase I Task 4) to get a clean
// slate.
// -----------------------------------------------------------------------

class EepromRoleTest : public ::testing::Test {
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
// Master flag
// -----------------------------------------------------------------------

TEST_F(EepromRoleTest, MasterFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadMasterFlag());
}

TEST_F(EepromRoleTest, MasterFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveMasterFlag(true);
  EXPECT_TRUE(mgr::loadMasterFlag());
  mgr::saveMasterFlag(false);
  EXPECT_FALSE(mgr::loadMasterFlag());
}

TEST_F(EepromRoleTest, MasterFlag_SkipSaveInDevMode) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(true);
  mgr::saveMasterFlag(true);
  // Should not be saved
  EXPECT_FALSE(mgr::loadMasterFlag());
}

// -----------------------------------------------------------------------
// Dev flag
// -----------------------------------------------------------------------

TEST_F(EepromRoleTest, DevFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadDevFlag());
}

TEST_F(EepromRoleTest, DevFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveDevFlag(true);
  EXPECT_TRUE(mgr::loadDevFlag());
  mgr::saveDevFlag(false);
  EXPECT_FALSE(mgr::loadDevFlag());
}
