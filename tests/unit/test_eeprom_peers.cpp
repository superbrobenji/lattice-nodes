#include <gtest/gtest.h>
#include <cstring>
#include "time_mock.h"
#include "persistence/eeprom/EepromCore.h"
#include "persistence/eeprom/EepromPeers.h"
#include "mesh/Mesh.h" // for PeerInfo and PEER_RECORD_SIZE constants
#include <nvs.h>

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

class EepromPeersTest : public ::testing::Test {
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
// Peer list — raw byte records (6 MAC + 32 public key = 38 bytes each)
// -----------------------------------------------------------------------

TEST_F(EepromPeersTest, PeerList_LoadWhenEmpty_FillsWithFF_ReturnsFalse) {
  namespace mgr = lattice::eeprom;

  uint8_t loadBuf[PEER_RECORD_SIZE]{};
  bool ok = mgr::loadPeerList(loadBuf, 1);

  EXPECT_FALSE(ok);
  // Verify buffer is filled with 0xFF
  for (size_t i = 0; i < PEER_RECORD_SIZE; ++i) {
    EXPECT_EQ(loadBuf[i], 0xFF);
  }
}

TEST_F(EepromPeersTest, PeerList_SaveAndLoad_SinglePeer) {
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

TEST_F(EepromPeersTest, PeerList_SaveAndLoad_MaxPeers) {
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

TEST_F(EepromPeersTest, PeerList_SaveZero_ClearsList) {
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

TEST_F(EepromPeersTest, PeerList_HasPeers_ReturnsFalseWhenEmpty) {
  EXPECT_FALSE(lattice::eeprom::hasPeers());
}

TEST_F(EepromPeersTest, PeerList_HasPeers_ReturnsTrueAfterSave) {
  namespace mgr = lattice::eeprom;
  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  mgr::savePeerList(peerRecord, 1);
  EXPECT_TRUE(mgr::hasPeers());
}

TEST_F(EepromPeersTest, PeerList_ClearPeerList_RemovesAllPeers) {
  namespace mgr = lattice::eeprom;

  uint8_t peerRecord[PEER_RECORD_SIZE]{};
  mgr::savePeerList(peerRecord, 1);
  EXPECT_TRUE(mgr::hasPeers());

  mgr::clearPeerList();
  EXPECT_FALSE(mgr::hasPeers());
}
