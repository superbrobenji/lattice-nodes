#include <gtest/gtest.h>
#include <cstring>
#include "persistence/EEPROM_Manager.h"
#include "Mesh/Mesh.h" // for PeerInfo and PEER_RECORD_SIZE constants

using planetopia::mesh::PeerInfo;
using planetopia::utils::EEPROM_Manager;
using planetopia::utils::EEPROM_SIZES::PEER_RECORD_SIZE;

// -----------------------------------------------------------------------
// Test fixture
//
// The EEPROM_Manager is a Meyers singleton — isInitialized stays true for
// the process lifetime. Between tests we:
//   1. clearAll()   — fills EEPROM with 0xFF and commits (via the manager)
//   2. EEPROM.reset() — resets the backing store + _commitCount to a clean
//                       blank-flash state, undoing the commit counter bump
// After this the manager's isInitialized is still true, which is fine:
// every operation still goes through the correct read/write path, and
// the backing store is blank (all 0xFF) just like factory flash.
// -----------------------------------------------------------------------

class EEPROMMgrTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    // Reset the backing store first so the singleton's init() sees blank flash
    EEPROM.reset();
    // init() is idempotent (returns early if isInitialized) — on first call it
    // begins EEPROM and writes the schema version.  Subsequent calls are no-ops.
    // To get a truly blank state we re-initialize the internal flag by using
    // clearAll() which fills 0xFF and commits, then reset the backing store
    // again to undo the schema-version byte left by init().
    auto& mgr = EEPROM_Manager::getInstance();
    mgr.init();     // Ensure initialized (no-op after first test)
    mgr.clearAll(); // Fill 0xFF + commit
    EEPROM.reset(); // Wipe commit counter and restore blank backing store
  }
};

// -----------------------------------------------------------------------
// Boot epoch
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, BootEpoch_StartsAtZeroWhenUnset) {
  // EEPROM is all 0xFF; loadBootEpoch() maps 0xFFFFFFFF → 0
  uint32_t epoch = EEPROM_Manager::getInstance().loadBootEpoch();
  EXPECT_EQ(epoch, 0u);
}

TEST_F(EEPROMMgrTest, BootEpoch_SaveAndLoad_RoundTrip) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.saveBootEpoch(12345);
  EXPECT_EQ(mgr.loadBootEpoch(), 12345u);
}

TEST_F(EEPROMMgrTest, BootEpoch_WrapsAtMax) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.saveBootEpoch(0xFFFFFFFE);
  EXPECT_EQ(mgr.loadBootEpoch(), 0xFFFFFFFEu);
}

// -----------------------------------------------------------------------
// Peer list — raw byte records (6 MAC + 32 public key = 38 bytes each)
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, PeerList_SaveAndLoad_EmptyList) {
  auto& mgr = EEPROM_Manager::getInstance();

  // Save zero peers (fills entire peer region with 0xFF)
  uint8_t emptyBuf[1]{}; // Not accessed when numPeers=0
  mgr.savePeerList(emptyBuf, 0);

  // Load zero peers back
  uint8_t loadBuf[PEER_RECORD_SIZE]{};
  bool ok = mgr.loadPeerList(loadBuf, 0);
  EXPECT_TRUE(ok);
}

TEST_F(EEPROMMgrTest, PeerList_SaveAndLoad_SinglePeer) {
  auto& mgr = EEPROM_Manager::getInstance();

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

  mgr.savePeerList(peerRecord, 1);

  uint8_t loaded[PEER_RECORD_SIZE]{};
  bool ok = mgr.loadPeerList(loaded, 1);

  EXPECT_TRUE(ok);
  EXPECT_EQ(loaded[0], 0xAA);
  EXPECT_EQ(loaded[1], 0xBB);
  EXPECT_EQ(loaded[5], 0xFF);
  EXPECT_EQ(loaded[6], 0x01);
  EXPECT_EQ(loaded[37], 0x7F);
}

TEST_F(EEPROMMgrTest, PeerList_SaveAndLoad_MaxPeers) {
  auto& mgr = EEPROM_Manager::getInstance();

  constexpr size_t MAX = 10;
  uint8_t peers[MAX * PEER_RECORD_SIZE]{};
  for (size_t i = 0; i < MAX; ++i) {
    peers[i * PEER_RECORD_SIZE] = static_cast<uint8_t>(i + 1); // Unique first MAC byte
  }

  mgr.savePeerList(peers, MAX);

  uint8_t loaded[MAX * PEER_RECORD_SIZE]{};
  bool ok = mgr.loadPeerList(loaded, MAX);

  EXPECT_TRUE(ok);
  for (size_t i = 0; i < MAX; ++i) {
    EXPECT_EQ(loaded[i * PEER_RECORD_SIZE], static_cast<uint8_t>(i + 1));
  }
}

// -----------------------------------------------------------------------
// Keypair CRC
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, Keypair_SaveAndLoad_ValidCRC) {
  auto& mgr = EEPROM_Manager::getInstance();

  uint8_t privKey[32]{}, pubKey[32]{};
  for (int i = 0; i < 32; ++i) {
    privKey[i] = static_cast<uint8_t>(i);
    pubKey[i] = static_cast<uint8_t>(i + 32);
  }

  mgr.saveKeypair(privKey, pubKey);

  uint8_t loadedPriv[32]{}, loadedPub[32]{};
  EXPECT_TRUE(mgr.loadKeypair(loadedPriv, loadedPub));
  EXPECT_EQ(memcmp(loadedPriv, privKey, 32), 0);
  EXPECT_EQ(memcmp(loadedPub, pubKey, 32), 0);
}

TEST_F(EEPROMMgrTest, Keypair_Load_CorruptedData_ReturnsFalse) {
  auto& mgr = EEPROM_Manager::getInstance();

  uint8_t priv[32];
  memset(priv, 42, 32);
  uint8_t pub[32];
  memset(pub, 99, 32);
  mgr.saveKeypair(priv, pub);

  // Corrupt one byte in the private key region
  using namespace planetopia::utils::EEPROM_ADDRESSES;
  EEPROM._data[PRIVATE_KEY + 5] ^= 0xFF;

  uint8_t p1[32]{}, p2[32]{};
  EXPECT_FALSE(mgr.loadKeypair(p1, p2));
}

TEST_F(EEPROMMgrTest, Keypair_Load_Unset_ReturnsFalse) {
  // Blank EEPROM — CRC mismatch expected
  uint8_t p1[32]{}, p2[32]{};
  EXPECT_FALSE(EEPROM_Manager::getInstance().loadKeypair(p1, p2));
}

// -----------------------------------------------------------------------
// Dirty flag + deferred flush
//
// savePeerList() calls markDirty() — no immediate commit.
// flushIfDirty() only commits after EEPROM_FLUSH_INTERVAL_MS (5000 ms).
// forceFlush() commits unconditionally.
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, DirtyFlag_NoFlushBeforeInterval) {
  auto& mgr = EEPROM_Manager::getInstance();
  int commitsBefore = EEPROM._commitCount;

  // savePeerList → markDirty(), no commit
  uint8_t peers[PEER_RECORD_SIZE]{};
  mgr.savePeerList(peers, 0);

  int commitsAfterSave = EEPROM._commitCount;
  EXPECT_EQ(commitsAfterSave, commitsBefore); // No commit on save

  // Advance time less than flush interval (5000 ms)
  advanceMillis(4000);
  mgr.flushIfDirty();
  EXPECT_EQ(EEPROM._commitCount, commitsAfterSave); // Still no flush
}

TEST_F(EEPROMMgrTest, DirtyFlag_FlushAfterInterval) {
  auto& mgr = EEPROM_Manager::getInstance();

  uint8_t peers[PEER_RECORD_SIZE]{};
  mgr.savePeerList(peers, 0);
  int commitsBefore = EEPROM._commitCount;

  advanceMillis(5001); // Past the 5000 ms flush interval
  mgr.flushIfDirty();

  EXPECT_GT(EEPROM._commitCount, commitsBefore);
}

TEST_F(EEPROMMgrTest, ForceFlush_CommitsImmediately) {
  auto& mgr = EEPROM_Manager::getInstance();

  uint8_t peers[PEER_RECORD_SIZE]{};
  mgr.savePeerList(peers, 0);
  int before = EEPROM._commitCount;

  mgr.forceFlush(); // Must commit regardless of time elapsed
  EXPECT_GT(EEPROM._commitCount, before);
}

// -----------------------------------------------------------------------
// TX power preset
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, TxPower_DefaultIsOutdoor) {
  // Blank EEPROM (0xFF) → default preset
  auto preset = EEPROM_Manager::getInstance().loadTxPowerPreset();
  EXPECT_EQ(preset, planetopia::config::TxPowerPreset::OUTDOOR);
}

TEST_F(EEPROMMgrTest, TxPower_SaveAndLoad) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.saveTxPowerPreset(planetopia::config::TxPowerPreset::INDOOR);
  EXPECT_EQ(mgr.loadTxPowerPreset(), planetopia::config::TxPowerPreset::INDOOR);
}

// -----------------------------------------------------------------------
// Node ID
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, NodeId_DefaultIsZero) {
  EXPECT_EQ(EEPROM_Manager::getInstance().loadNodeId(), 0u);
}

TEST_F(EEPROMMgrTest, NodeId_SaveAndLoad) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.saveNodeId(42);
  mgr.forceFlush();
  EXPECT_EQ(mgr.loadNodeId(), 42u);
}

TEST_F(EEPROMMgrTest, NodeId_SaveZeroRoundtrips) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.saveNodeId(7);
  mgr.saveNodeId(0);
  mgr.forceFlush();
  EXPECT_EQ(mgr.loadNodeId(), 0u);
}

// -----------------------------------------------------------------------
// TOFU secondary master MAC (EEPROM layout v3)
// -----------------------------------------------------------------------

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_UnsetReturnsFalse) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  uint8_t mac[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(mac);
  EXPECT_FALSE(found) << "Blank EEPROM must report secondary master MAC as unset";
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_SaveAndLoad_RoundTrip) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  const uint8_t expected[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  mgr.saveKnownMasterMacSecondary(expected);

  uint8_t loaded[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(loaded);

  EXPECT_TRUE(found);
  EXPECT_EQ(memcmp(loaded, expected, 6), 0);
}

TEST_F(EEPROMMgrTest, KnownMasterMacSecondary_Clear_ResetsToUnset) {
  auto& mgr = EEPROM_Manager::getInstance();
  mgr.init();
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  mgr.saveKnownMasterMacSecondary(mac);
  mgr.clearKnownMasterMacSecondary();

  uint8_t loaded[6] = {};
  bool found = mgr.loadKnownMasterMacSecondary(loaded);
  EXPECT_FALSE(found) << "After clear, secondary master MAC must report unset";
}
