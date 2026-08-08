#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include "persistence/eeprom/EepromCore.h"
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

class EepromIdentityTest : public ::testing::Test {
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
// Keypair CRC
// -----------------------------------------------------------------------

TEST_F(EepromIdentityTest, Keypair_SaveAndLoad_ValidCRC) {
  namespace mgr = lattice::eeprom;

  uint8_t privKey[32]{}, pubKey[32]{};
  for (int i = 0; i < 32; ++i) {
    privKey[i] = static_cast<uint8_t>(i);
    pubKey[i] = static_cast<uint8_t>(i + 32);
  }

  mgr::saveKeypair(privKey, pubKey);

  uint8_t loadedPriv[32]{}, loadedPub[32]{};
  EXPECT_TRUE(mgr::loadKeypair(loadedPriv, loadedPub));
  EXPECT_EQ(memcmp(loadedPriv, privKey, 32), 0);
  EXPECT_EQ(memcmp(loadedPub, pubKey, 32), 0);
}

TEST_F(EepromIdentityTest, Keypair_Load_NotFound_ReturnsFalse) {
  // No keypair saved — should return false
  uint8_t p1[32]{}, p2[32]{};
  EXPECT_FALSE(lattice::eeprom::loadKeypair(p1, p2));
}

TEST_F(EepromIdentityTest, Keypair_Load_CorruptedData_ReturnsFalse) {
  namespace mgr = lattice::eeprom;

  uint8_t priv[32];
  memset(priv, 42, 32);
  uint8_t pub[32];
  memset(pub, 99, 32);
  mgr::saveKeypair(priv, pub);

  // Manually corrupt the CRC in the NVS store
  NvsMock::_store["lattice/" + std::string(lattice::utils::NVS_KEYS::KEYPAIR_CRC)] = {0xFF, 0xFF,
                                                                                      0xFF, 0xFF};

  uint8_t p1[32]{}, p2[32]{};
  EXPECT_FALSE(mgr::loadKeypair(p1, p2));
}

// -----------------------------------------------------------------------
// Node ID
// -----------------------------------------------------------------------

TEST_F(EepromIdentityTest, NodeId_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadNodeId(), 0u);
}

TEST_F(EepromIdentityTest, NodeId_SaveAndLoad) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  EXPECT_EQ(mgr::loadNodeId(), 42u);
}

TEST_F(EepromIdentityTest, NodeId_SaveZeroRoundtrips) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(7);
  mgr::saveNodeId(0);
  EXPECT_EQ(mgr::loadNodeId(), 0u);
}
