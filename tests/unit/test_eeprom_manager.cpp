#include <gtest/gtest.h>
#include <cstring>
#include "persistence/EepromManager.h"
#include "mesh/Mesh.h" // for PeerInfo and PEER_RECORD_SIZE constants
#include <nvs.h>
#include "error/Error.h"

using lattice::mesh::PeerInfo;
using lattice::utils::EEPROM_SIZES::PEER_RECORD_SIZE;

// -----------------------------------------------------------------------
// Test fixture
//
// lattice::eeprom holds its state in file-static storage (Phase H2 item AA:
// migrated off the old Meyers-singleton EepromManager class) — isInitialized
// stays true for the process lifetime. Between tests we clear the
// NvsMock static store (tests/mocks/nvs.h, Phase I Task 4) to get a clean
// slate.
// -----------------------------------------------------------------------

class EEPROMMgrTest : public ::testing::Test {
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

TEST_F(EEPROMMgrTest, Init_SucceedsFirstTime) {
  // Already initialized in SetUp
  EXPECT_TRUE(lattice::eeprom::init());
}

TEST_F(EEPROMMgrTest, Init_IdempotentOnSecondCall) {
  namespace mgr = lattice::eeprom;
  EXPECT_TRUE(mgr::init());
  EXPECT_TRUE(mgr::init()); // Second call should return true
}

// -----------------------------------------------------------------------
// Boot epoch
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, BootEpoch_StartsAtZeroWhenUnset) {
  uint32_t epoch = lattice::eeprom::loadBootEpoch();
  EXPECT_EQ(epoch, 0u);
}

TEST_F(EEPROMMgrTest, BootEpoch_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveBootEpoch(12345);
  EXPECT_EQ(mgr::loadBootEpoch(), 12345u);
}

TEST_F(EEPROMMgrTest, BootEpoch_WrapsAtMax) {
  namespace mgr = lattice::eeprom;
  mgr::saveBootEpoch(0xFFFFFFFE);
  EXPECT_EQ(mgr::loadBootEpoch(), 0xFFFFFFFEu);
}

// -----------------------------------------------------------------------
// Master flag
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, MasterFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadMasterFlag());
}

TEST_F(EEPROMMgrTest, MasterFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveMasterFlag(true);
  EXPECT_TRUE(mgr::loadMasterFlag());
  mgr::saveMasterFlag(false);
  EXPECT_FALSE(mgr::loadMasterFlag());
}

TEST_F(EEPROMMgrTest, MasterFlag_SkipSaveInDevMode) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(true);
  mgr::saveMasterFlag(true);
  // Should not be saved
  EXPECT_FALSE(mgr::loadMasterFlag());
}

// -----------------------------------------------------------------------
// Dev flag
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, DevFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadDevFlag());
}

TEST_F(EEPROMMgrTest, DevFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveDevFlag(true);
  EXPECT_TRUE(mgr::loadDevFlag());
  mgr::saveDevFlag(false);
  EXPECT_FALSE(mgr::loadDevFlag());
}

// -----------------------------------------------------------------------
// Mesh key
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, MeshKey_SaveAndLoad_RoundTrip) {
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

TEST_F(EEPROMMgrTest, MeshKey_Load_NotFound_ReturnsFalse) {
  uint8_t key[16]{};
  EXPECT_FALSE(lattice::eeprom::loadMeshKey(key, 16));
}

TEST_F(EEPROMMgrTest, MeshKey_WrongSize_ReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t key[16]{};
  EXPECT_FALSE(mgr::loadMeshKey(key, 8)); // Wrong size
}

// -----------------------------------------------------------------------
// Peer list — raw byte records (6 MAC + 32 public key = 38 bytes each)
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, PeerList_LoadWhenEmpty_FillsWithFF_ReturnsFalse) {
  namespace mgr = lattice::eeprom;

  uint8_t loadBuf[PEER_RECORD_SIZE]{};
  bool ok = mgr::loadPeerList(loadBuf, 1);

  EXPECT_FALSE(ok);
  // Verify buffer is filled with 0xFF
  for (size_t i = 0; i < PEER_RECORD_SIZE; ++i) {
    EXPECT_EQ(loadBuf[i], 0xFF);
  }
}

TEST_F(EEPROMMgrTest, PeerList_SaveAndLoad_SinglePeer) {
  namespace mgr = lattice::eeprom;

  // Build a 38-byte peer record: 6-byte MAC + 32-byte public key
  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  peerRecord[0] = 0xAA;
  peerRecord[1] = 0xBB;
  peerRecord[2] = 0xCC;
  peerRecord[3] = 0xDD;
  peerRecord[4] = 0xEE;
  peerRecord[5] = 0xFF;
  peerRecord[6] = 0x01;  // publicKey[0]
  peerRecord[37] = 0x7F; // publicKey[31]

  mgr::savePeerList(peerRecord, 1);

  uint8_t loaded[PEER_RECORD_SIZE]{};
  bool ok = mgr::loadPeerList(loaded, 1);

  EXPECT_TRUE(ok);
  EXPECT_EQ(loaded[0], 0xAA);
  EXPECT_EQ(loaded[1], 0xBB);
  EXPECT_EQ(loaded[5], 0xFF);
  EXPECT_EQ(loaded[6], 0x01);
  EXPECT_EQ(loaded[37], 0x7F);
}

TEST_F(EEPROMMgrTest, PeerList_SaveAndLoad_MaxPeers) {
  namespace mgr = lattice::eeprom;

  constexpr size_t MAX = 10;
  uint8_t peers[MAX * PEER_RECORD_SIZE]{};
  for (size_t i = 0; i < MAX; ++i) {
    peers[i * PEER_RECORD_SIZE] = static_cast<uint8_t>(i + 1); // Unique first MAC byte
  }

  mgr::savePeerList(peers, MAX);

  uint8_t loaded[MAX * PEER_RECORD_SIZE]{};
  bool ok = mgr::loadPeerList(loaded, MAX);

  EXPECT_TRUE(ok);
  for (size_t i = 0; i < MAX; ++i) {
    EXPECT_EQ(loaded[i * PEER_RECORD_SIZE], static_cast<uint8_t>(i + 1));
  }
}

TEST_F(EEPROMMgrTest, PeerList_SaveZero_ClearsList) {
  namespace mgr = lattice::eeprom;

  // First save some peers
  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  peerRecord[0] = 0xAA;
  mgr::savePeerList(peerRecord, 1);
  EXPECT_TRUE(mgr::hasPeers());

  // Now save zero peers
  mgr::savePeerList(peerRecord, 0);
  EXPECT_FALSE(mgr::hasPeers());
}

TEST_F(EEPROMMgrTest, PeerList_HasPeers_ReturnsFalseWhenEmpty) {
  EXPECT_FALSE(lattice::eeprom::hasPeers());
}

TEST_F(EEPROMMgrTest, PeerList_HasPeers_ReturnsTrueAfterSave) {
  namespace mgr = lattice::eeprom;
  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  mgr::savePeerList(peerRecord, 1);
  EXPECT_TRUE(mgr::hasPeers());
}

TEST_F(EEPROMMgrTest, PeerList_ClearPeerList_RemovesAllPeers) {
  namespace mgr = lattice::eeprom;

  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  mgr::savePeerList(peerRecord, 1);
  EXPECT_TRUE(mgr::hasPeers());

  mgr::clearPeerList();
  EXPECT_FALSE(mgr::hasPeers());
}

// -----------------------------------------------------------------------
// Adapter type
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, AdapterType_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadAdapterType(), 0u);
}

TEST_F(EEPROMMgrTest, AdapterType_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveAdapterType(3);
  EXPECT_EQ(mgr::loadAdapterType(), 3u);
}

// -----------------------------------------------------------------------
// Reboot tracking
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, RebootCount_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadRebootCount(), 0u);
}

TEST_F(EEPROMMgrTest, RebootCount_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveRebootCount(5);
  EXPECT_EQ(mgr::loadRebootCount(), 5u);
}

TEST_F(EEPROMMgrTest, RebootReason_DefaultIs0xFF) {
  EXPECT_EQ(lattice::eeprom::loadRebootReason(), 0xFFu);
}

TEST_F(EEPROMMgrTest, RebootReason_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveRebootReason(42);
  EXPECT_EQ(mgr::loadRebootReason(), 42u);
}

// -----------------------------------------------------------------------
// Keypair CRC
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, Keypair_SaveAndLoad_ValidCRC) {
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

TEST_F(EEPROMMgrTest, Keypair_Load_NotFound_ReturnsFalse) {
  // No keypair saved — should return false
  uint8_t p1[32]{}, p2[32]{};
  EXPECT_FALSE(lattice::eeprom::loadKeypair(p1, p2));
}

TEST_F(EEPROMMgrTest, Keypair_Load_CorruptedData_ReturnsFalse) {
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
// Enrolled flag
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, EnrolledFlag_DefaultsToFalse) {
  EXPECT_FALSE(lattice::eeprom::loadEnrolledFlag());
}

TEST_F(EEPROMMgrTest, EnrolledFlag_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  mgr::saveEnrolledFlag(true);
  EXPECT_TRUE(mgr::loadEnrolledFlag());
  mgr::saveEnrolledFlag(false);
  EXPECT_FALSE(mgr::loadEnrolledFlag());
}

// -----------------------------------------------------------------------
// Known master MAC
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, KnownMasterMac_UnsetReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t mac[6] = {};
  bool found = mgr::loadKnownMasterMac(mac);
  EXPECT_FALSE(found);
}

TEST_F(EEPROMMgrTest, KnownMasterMac_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr::saveKnownMasterMac(expected);

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMac(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EEPROMMgrTest, KnownMasterMac_Clear_ResetsToUnset) {
  namespace mgr = lattice::eeprom;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr::saveKnownMasterMac(mac);
  mgr::clearKnownMasterMac();

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMac(loaded);
  EXPECT_FALSE(found);
}

TEST_F(EEPROMMgrTest, KnownMasterMac_AllFF_TreatedAsUnset) {
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

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_UnsetReturnsFalse) {
  namespace mgr = lattice::eeprom;
  uint8_t mac[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(mac);
  EXPECT_FALSE(found);
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_SaveAndLoad_RoundTrip) {
  namespace mgr = lattice::eeprom;
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr::saveKnownMasterMacSecondary(expected);

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_Clear_ResetsToUnset) {
  namespace mgr = lattice::eeprom;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr::saveKnownMasterMacSecondary(mac);
  mgr::clearKnownMasterMacSecondary();

  uint8_t loaded[6] = {};
  bool found = mgr::loadKnownMasterMacSecondary(loaded);
  EXPECT_FALSE(found);
}

// -----------------------------------------------------------------------
// TX power preset
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, TxPower_DefaultIsOutdoor) {
  auto preset = lattice::eeprom::loadTxPowerPreset();
  EXPECT_EQ(preset, lattice::config::TxPowerPreset::OUTDOOR);
}

TEST_F(EEPROMMgrTest, TxPower_SaveAndLoad) {
  namespace mgr = lattice::eeprom;
  mgr::saveTxPowerPreset(lattice::config::TxPowerPreset::INDOOR);
  EXPECT_EQ(mgr::loadTxPowerPreset(), lattice::config::TxPowerPreset::INDOOR);
}

// -----------------------------------------------------------------------
// Node ID
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, NodeId_DefaultIsZero) {
  EXPECT_EQ(lattice::eeprom::loadNodeId(), 0u);
}

TEST_F(EEPROMMgrTest, NodeId_SaveAndLoad) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  EXPECT_EQ(mgr::loadNodeId(), 42u);
}

TEST_F(EEPROMMgrTest, NodeId_SaveZeroRoundtrips) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(7);
  mgr::saveNodeId(0);
  EXPECT_EQ(mgr::loadNodeId(), 0u);
}

// -----------------------------------------------------------------------
// Clear all
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, ClearAll_RemovesAllData) {
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

TEST_F(EEPROMMgrTest, FlushIfDirty_IsNoOp) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  mgr::flushIfDirty();               // Should be a no-op
  EXPECT_EQ(mgr::loadNodeId(), 42u); // Data should already be persisted
}

TEST_F(EEPROMMgrTest, ForceFlush_IsNoOp) {
  namespace mgr = lattice::eeprom;
  mgr::saveNodeId(42);
  mgr::forceFlush();                 // Should be a no-op
  EXPECT_EQ(mgr::loadNodeId(), 42u); // Data should already be persisted
}

// -----------------------------------------------------------------------
// Boot epoch — DEV-mode RAM-only seed (issue #43)
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, SaveBootEpoch_DevMode_UsesRAMSeed) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(true);
  mgr::saveBootEpoch(5);
  EXPECT_EQ(mgr::loadBootEpoch(), 5u);
  mgr::saveBootEpoch(7);
  EXPECT_EQ(mgr::loadBootEpoch(), 7u);
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_DevMode_DoesNotTouchNVS) {
  namespace mgr = lattice::eeprom;
  NvsMock::_store.clear();
  mgr::setDevMode(true);
  mgr::saveBootEpoch(42);
  // NVS store must be empty — DEV never persists.
  EXPECT_TRUE(NvsMock::_store.empty());
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_Persists) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  mgr::saveBootEpoch(9);
  EXPECT_EQ(mgr::loadBootEpoch(), 9u);
}

// -----------------------------------------------------------------------
// Tiered NVS write-return escalation (issue #43)
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_ShortWrite_Fatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  NvsMock::_failNextWriteKey =
      lattice::utils::NVS_KEYS::BOOT_EPOCH; // mock: next putUInt for this key returns 0
  EXPECT_THROW(mgr::saveBootEpoch(1), lattice::err::FatalError);
  NvsMock::_failNextWriteKey = nullptr;
}

TEST_F(EEPROMMgrTest, SaveKnownMasterMac_ShortWrite_Fatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  NvsMock::_failNextWriteKey = lattice::utils::NVS_KEYS::KNOWN_MASTER_MAC;
  EXPECT_THROW(mgr::saveKnownMasterMac(mac), lattice::err::FatalError);
  NvsMock::_failNextWriteKey = nullptr;
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_FullWrite_NoFatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  int before = lattice_test_errFailCount;
  mgr::saveBootEpoch(3);
  EXPECT_EQ(lattice_test_errFailCount, before);
  EXPECT_EQ(mgr::loadBootEpoch(), 3u);
}

TEST_F(EEPROMMgrTest, SaveRebootCount_ShortWrite_WarnsNoFatal) {
  namespace mgr = lattice::eeprom;
  mgr::setDevMode(false);
  int before = lattice_test_errFailCount;
  NvsMock::_failNextWriteKey = lattice::utils::NVS_KEYS::REBOOT_COUNT;
  mgr::saveRebootCount(1); // Non-security setter: must NOT escalate
  EXPECT_EQ(lattice_test_errFailCount, before);
  NvsMock::_failNextWriteKey = nullptr;
}
