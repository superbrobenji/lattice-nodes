#include <gtest/gtest.h>
#include <cstring>
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromDiagnostics.h"
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

class EepromDiagnosticsTest : public ::testing::Test {
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
// Boot epoch
// -----------------------------------------------------------------------

TEST_F(EepromDiagnosticsTest, BootEpoch_StartsAtZeroWhenUnset) {
  uint32_t epoch = lattice::eeprom::loadBootEpoch();
  EXPECT_EQ(epoch, 0u);
}

TEST_F(EepromDiagnosticsTest, BootEpoch_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveBootEpoch(12345);
  EXPECT_EQ(mgr::loadBootEpoch(), 12345u);
}

TEST_F(EepromDiagnosticsTest, BootEpoch_WrapsAtMax) {
  namespace mgr = lattice::eeprom;
  mgr::saveBootEpoch(0xFFFFFFFE);
  EXPECT_EQ(mgr::loadBootEpoch(), 0xFFFFFFFEu);
}

// -----------------------------------------------------------------------
// Reboot tracking
// -----------------------------------------------------------------------

TEST_F(EepromDiagnosticsTest, RebootCount_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadRebootCount(), 0u);
}

TEST_F(EepromDiagnosticsTest, RebootCount_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveRebootCount(5);
  EXPECT_EQ(mgr::loadRebootCount(), 5u);
}

TEST_F(EepromDiagnosticsTest, RebootReason_DefaultIs0xFF) {
  EXPECT_EQ(lattice::eeprom::loadRebootReason(), 0xFFu);
}

TEST_F(EepromDiagnosticsTest, RebootReason_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveRebootReason(42);
  EXPECT_EQ(mgr::loadRebootReason(), 42u);
}

// -----------------------------------------------------------------------
// Boot epoch — DEV-mode RAM-only seed (issue #43)
// -----------------------------------------------------------------------

TEST_F(EepromDiagnosticsTest, SaveBootEpoch_DevMode_UsesRAMSeed) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(true);
  mgr::saveBootEpoch(5);
  EXPECT_EQ(mgr::loadBootEpoch(), 5u);
  mgr::saveBootEpoch(7);
  EXPECT_EQ(mgr::loadBootEpoch(), 7u);
}

TEST_F(EepromDiagnosticsTest, SaveBootEpoch_DevMode_DoesNotTouchNVS) {
  namespace mgr = lattice::eeprom;
  NvsMock::_store.clear();
  mgr::setDevMode(true);
  mgr::saveBootEpoch(42);
  // NVS store must be empty — DEV never persists.
  EXPECT_TRUE(NvsMock::_store.empty());
}

TEST_F(EepromDiagnosticsTest, SaveBootEpoch_ProdMode_Persists) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  mgr::saveBootEpoch(9);
  EXPECT_EQ(mgr::loadBootEpoch(), 9u);
}

// -----------------------------------------------------------------------
// Tiered NVS write-return escalation (issue #43)
// -----------------------------------------------------------------------

TEST_F(EepromDiagnosticsTest, SaveBootEpoch_ProdMode_ShortWrite_Fatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  NvsMock::_failNextWriteKey =
      lattice::utils::NVS_KEYS::BOOT_EPOCH; // mock: next putUInt for this key returns 0
  EXPECT_THROW(mgr::saveBootEpoch(1), lattice::err::FatalError);
  NvsMock::_failNextWriteKey = nullptr;
}

TEST_F(EepromDiagnosticsTest, SaveBootEpoch_ProdMode_FullWrite_NoFatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  int before = lattice_test_errFailCount;
  mgr::saveBootEpoch(3);
  EXPECT_EQ(lattice_test_errFailCount, before);
  EXPECT_EQ(mgr::loadBootEpoch(), 3u);
}

TEST_F(EepromDiagnosticsTest, SaveRebootCount_ShortWrite_WarnsNoFatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  int before = lattice_test_errFailCount;
  NvsMock::_failNextWriteKey = lattice::utils::NVS_KEYS::REBOOT_COUNT;
  mgr::saveRebootCount(1); // Non-security setter: must NOT escalate
  EXPECT_EQ(lattice_test_errFailCount, before);
  NvsMock::_failNextWriteKey = nullptr;
}
