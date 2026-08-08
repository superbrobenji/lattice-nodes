#include <gtest/gtest.h>
#include <cstring>
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromEnrollment.h"
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

class EepromEnrollmentTest : public ::testing::Test {
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
// Enrolled flag
// -----------------------------------------------------------------------

TEST_F(EepromEnrollmentTest, EnrolledFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadEnrolledFlag());
}

TEST_F(EepromEnrollmentTest, EnrolledFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveEnrolledFlag(true);
  EXPECT_TRUE(mgr::loadEnrolledFlag());
  mgr::saveEnrolledFlag(false);
  EXPECT_FALSE(mgr::loadEnrolledFlag());
}
