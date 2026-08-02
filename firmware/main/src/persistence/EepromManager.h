#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "src/logging/Logger.h"
#include "../../project_config.h"

namespace lattice {
namespace utils {

// NVS key constants
namespace NVS_KEYS {
constexpr const char* MASTER_FLAG = "master";
constexpr const char* DEV_FLAG = "dev";
constexpr const char* ADAPTER_TYPE = "adapter";
constexpr const char* MESH_KEY = "meshkey";
constexpr const char* PEER_LIST = "peers";
constexpr const char* REBOOT_REASON = "rboot_rsn";
constexpr const char* REBOOT_COUNT = "rboot_cnt";
constexpr const char* PRIVATE_KEY = "privkey";
constexpr const char* PUBLIC_KEY = "pubkey";
constexpr const char* KEYPAIR_CRC = "keypair_crc";
constexpr const char* ENROLLED_FLAG = "enrolled";
constexpr const char* BOOT_EPOCH = "boot_epoch";
constexpr const char* KNOWN_MASTER_MAC = "master_mac";
constexpr const char* KNOWN_MASTER_MAC_SECONDARY = "master_mac2";
constexpr const char* TX_POWER_PRESET = "tx_power";
constexpr const char* NODE_ID = "node_id";
} // namespace NVS_KEYS

// EEPROM size constants - retained for compatibility with other modules
namespace EEPROM_SIZES {
constexpr uint8_t MESH_KEY_SIZE = 16;
constexpr uint8_t MAX_PEERS = 10;
constexpr uint8_t PEER_MAC_SIZE = 6;
constexpr uint8_t PEER_PUBLIC_KEY_SIZE = 32;
constexpr uint8_t PEER_RECORD_SIZE = PEER_MAC_SIZE + PEER_PUBLIC_KEY_SIZE; // 38 bytes
constexpr uint16_t PEER_LIST_SIZE = MAX_PEERS * PEER_RECORD_SIZE;          // 380 bytes
} // namespace EEPROM_SIZES

class EepromManager {
private:
  bool isInitialized;
  bool isDevMode;
  Preferences _prefs;

  // Private constructor for singleton pattern
  EepromManager();

  // Helper methods
  bool ensureInitialized();
  void logOperation(const char* operation, const char* details = nullptr);

public:
  // Singleton pattern
  static EepromManager& getInstance();

  // Delete copy and move to enforce singleton
  EepromManager(const EepromManager&) = delete;
  EepromManager& operator=(const EepromManager&) = delete;
  EepromManager(EepromManager&&) = delete;
  EepromManager& operator=(EepromManager&&) = delete;

  // Initialization and configuration
  bool init();
  void setDevMode(bool devMode);
  bool getDevMode() const;

  // Master flag operations
  bool loadMasterFlag();
  void saveMasterFlag(bool isMaster);

  // Dev flag operations
  bool loadDevFlag();
  void saveDevFlag(bool isDev);

  // Mesh key operations
  bool loadMeshKey(uint8_t* key, size_t keySize);
  void saveMeshKey(const uint8_t* key, size_t keySize);

  // Peer list operations — each record is PEER_RECORD_SIZE bytes (6 MAC + 32 public key)
  bool loadPeerList(uint8_t* peerRecords, size_t maxPeers);
  void savePeerList(const uint8_t* peerRecords, size_t numPeers);
  bool hasPeers();
  void clearPeerList();

  // Adapter type operations
  uint8_t loadAdapterType();
  void saveAdapterType(uint8_t adapterType);

  // Reboot tracking operations
  uint8_t loadRebootCount();
  void saveRebootCount(uint8_t count);
  void saveRebootReason(uint8_t reason);
  uint8_t loadRebootReason();

  // Keypair operations
  bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32);
  void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32);
  bool loadEnrolledFlag();
  void saveEnrolledFlag(bool enrolled);

  // Boot epoch for replay protection
  uint32_t loadBootEpoch();
  void saveBootEpoch(uint32_t epoch);

  // TOFU master MAC — persisted so a power cycle preserves the known master
  bool loadKnownMasterMac(uint8_t* mac);
  void saveKnownMasterMac(const uint8_t* mac);
  void clearKnownMasterMac();

  // TOFU secondary master MAC — persisted secondary master for dual-master mode
  bool loadKnownMasterMacSecondary(uint8_t* mac);
  void saveKnownMasterMacSecondary(const uint8_t* mac);
  void clearKnownMasterMacSecondary();

  // TX power preset — deployment-specific, persisted across reboots
  lattice::config::TxPowerPreset loadTxPowerPreset();
  void saveTxPowerPreset(lattice::config::TxPowerPreset preset);

  // Node ID — logical node ID assigned by server (0 = unset)
  uint8_t loadNodeId();
  void saveNodeId(uint8_t nodeId);

  // Deferred flush API (no-ops for NVS)
  void flushIfDirty();
  void forceFlush();

  // Utility operations
  void clearAll();

  // Debug and diagnostics
  void dumpEEPROM();

  // Destructor
  ~EepromManager();

#ifdef UNIT_TEST
  // Test-only accessor: exposes internal init state so the e2e harness can verify
  // a fresh NodeContext's swapIn restores pristine singleton state rather than
  // silently inheriting a previous node's initialized flag.
  bool isInitializedForTest() const { return isInitialized; }
#endif
};

} // namespace utils
} // namespace lattice

#endif // EEPROM_MANAGER_H
