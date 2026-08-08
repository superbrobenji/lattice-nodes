#include <gtest/gtest.h>
#include <cstring>
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromRole.h"
#include "persistence/eeprom/EepromIdentity.h"
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

class EepromCoreTest : public ::testing::Test {
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
// Initialization
// -----------------------------------------------------------------------

TEST_F(EepromCoreTest, Init_SucceedsFirstTime) {
  // Already initialized in SetUp
  EXPECT_TRUE(lattice::eeprom::init());
}

TEST_F(EepromCoreTest, Init_IdempotentOnSecondCall) {
  namespace mgr = lattice::eeprom;
  EXPECT_TRUE(mgr::init());
  EXPECT_TRUE(mgr::init()); // Second call should return true
}

// -----------------------------------------------------------------------
// Clear all
// -----------------------------------------------------------------------

TEST_F(EepromCoreTest, ClearAll_RemovesAllData) {
  namespace mgr = lattice::eeprom;

  // Save various data
  mgr::saveMasterFlag(true);
  mgr::saveDevFlag(true);
  mgr::saveNodeId(42);

  mgr::clearAll();

  // Verify all data is cleared
  EXPECT_FALSE(mgr::loadMasterFlag());
  EXPECT_FALSE(mgr::loadDevFlag());
  EXPECT_EQ(mgr::loadNodeId(), 0u);
}

// -----------------------------------------------------------------------
// Deferred flush API (no-ops for NVS)
// -----------------------------------------------------------------------

TEST_F(EepromCoreTest, FlushIfDirty_IsNoOp) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  mgr::flushIfDirty();               // Should be a no-op
  EXPECT_EQ(mgr::loadNodeId(), 42u); // Data should already be persisted
}

TEST_F(EepromCoreTest, ForceFlush_IsNoOp) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  mgr::forceFlush();                 // Should be a no-op
  EXPECT_EQ(mgr::loadNodeId(), 42u); // Data should already be persisted
}
