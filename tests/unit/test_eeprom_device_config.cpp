#include <gtest/gtest.h>
#include <cstring>
#include "time_mock.h"
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromDeviceConfig.h"
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

class EepromDeviceConfigTest : public ::testing::Test {
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
// Adapter type
// -----------------------------------------------------------------------

TEST_F(EepromDeviceConfigTest, AdapterType_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadAdapterType(), 0u);
}

TEST_F(EepromDeviceConfigTest, AdapterType_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveAdapterType(3);
  EXPECT_EQ(mgr::loadAdapterType(), 3u);
}

// -----------------------------------------------------------------------
// TX power preset
// -----------------------------------------------------------------------

TEST_F(EepromDeviceConfigTest, TxPower_DefaultIsOutdoor) {
  auto preset = lattice::eeprom::loadTxPowerPreset();
  EXPECT_EQ(preset, lattice::config::TxPowerPreset::OUTDOOR);
}

TEST_F(EepromDeviceConfigTest, TxPower_SaveAndLoad) {
  namespace mgr = lattice::eeprom;
  mgr::saveTxPowerPreset(lattice::config::TxPowerPreset::INDOOR);
  EXPECT_EQ(mgr::loadTxPowerPreset(), lattice::config::TxPowerPreset::INDOOR);
}
