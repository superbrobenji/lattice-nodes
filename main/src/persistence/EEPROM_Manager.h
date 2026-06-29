#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <EEPROM.h>
#include "src/core/Logger.h"
#include "../../project_config.h"

namespace planetopia {
namespace utils {

// EEPROM address constants - centralized in one place
// Layout (v2 — Tasks 10-11 restructured keypair/reboot tracking; Task 18 adds formal versioning):
//   0   MASTER_FLAG      (1 byte)
//   1   DEV_FLAG         (1 byte)
//   8   ADAPTER_TYPE     (1 byte)
//  16   MESH_KEY         (16 bytes, ends 31)
//  32   PEER_LIST        (380 bytes: 10 × 38B records, ends 411)
// 412   REBOOT_REASON    (1 byte)
// 413   REBOOT_COUNT     (1 byte)
// 414   RESERVED         (3 bytes: 414-416)
// 417   PRIVATE_KEY      (32 bytes, ends 448)
// 449   PUBLIC_KEY       (32 bytes, ends 480)
// 481   KEYPAIR_CRC      (2 bytes, ends 482)
// 483   ENROLLED_FLAG    (1 byte)
// 484   BOOT_EPOCH       (4 bytes, ends 487) — boot count for replay protection
// 488   KNOWN_MASTER_MAC (6 bytes, ends 493) — TOFU master MAC (0xFF×6 = unset)
// 494   SCHEMA_VERSION   (1 byte) — EEPROM layout version for migration gating
// 495   TX_POWER_PRESET  (1 byte) — TxPowerPreset enum value (0=SHORT_RANGE 1=INDOOR 2=OUTDOOR)
// 496   NODE_ID          (1 byte) — logical node ID assigned by server (0 = unset, 0xFF = erased)
// Total used: 497 bytes — fits in 512
namespace EEPROM_ADDRESSES {
constexpr uint16_t MASTER_FLAG = 0;        // Master flag (1 byte)
constexpr uint16_t DEV_FLAG = 1;           // Dev mode flag (1 byte)
constexpr uint16_t ADAPTER_TYPE = 8;       // Adapter type (1 byte)
constexpr uint16_t MESH_KEY = 16;          // Mesh encryption key (16 bytes)
constexpr uint16_t PEER_LIST = 32;         // Peer records: 10 × (6 MAC + 32 pubkey) = 380 bytes
constexpr uint16_t REBOOT_REASON = 412;    // 1 byte: last reset reason
constexpr uint16_t REBOOT_COUNT = 413;     // 1 byte: consecutive unexpected reboot count
constexpr uint16_t RESERVED = 414;         // Reserved for future use (3 bytes: 414-416)
constexpr uint16_t PRIVATE_KEY = 417;      // 32 bytes: Curve25519 private key
constexpr uint16_t PUBLIC_KEY = 449;       // 32 bytes: Curve25519 public key
constexpr uint16_t KEYPAIR_CRC = 481;      // 2 bytes: CRC16 over private+public key
constexpr uint16_t ENROLLED_FLAG = 483;    // 1 byte: 0x01 = enrolled, 0xFF = not enrolled
constexpr uint16_t BOOT_EPOCH = 484;       // 4 bytes: boot count for replay protection (ends 487)
constexpr uint16_t KNOWN_MASTER_MAC = 488; // 6 bytes: TOFU master MAC (0xFF×6 = unset, ends 493)
constexpr uint16_t SCHEMA_VERSION = 494;   // 1 byte: EEPROM layout version for migration gating
constexpr uint16_t TX_POWER_PRESET =
    495;                          // 1 byte: TxPowerPreset (0=SHORT_RANGE 1=INDOOR 2=OUTDOOR)
constexpr uint16_t NODE_ID = 496; // 1 byte: logical node ID assigned by server (0 = unset)

// Old v1 addresses (used only during migration in EEPROM_Manager::init())
constexpr uint16_t V1_REBOOT_REASON = 92;
constexpr uint16_t V1_REBOOT_COUNT = 93;
constexpr uint16_t V1_PRIVATE_KEY = 97;
constexpr uint16_t V1_PUBLIC_KEY = 129;
constexpr uint16_t V1_KEYPAIR_CRC = 161;
constexpr uint16_t V1_ENROLLED_FLAG = 163;
} // namespace EEPROM_ADDRESSES

// EEPROM size constants
namespace EEPROM_SIZES {
constexpr uint16_t TOTAL_SIZE = 512;
constexpr uint8_t MESH_KEY_SIZE = 16;
constexpr uint8_t MAX_PEERS = 10;
constexpr uint8_t PEER_MAC_SIZE = 6;
constexpr uint8_t PEER_PUBLIC_KEY_SIZE = 32;
constexpr uint8_t PEER_RECORD_SIZE = PEER_MAC_SIZE + PEER_PUBLIC_KEY_SIZE; // 38 bytes
constexpr uint16_t PEER_LIST_SIZE = MAX_PEERS * PEER_RECORD_SIZE;          // 380 bytes
constexpr uint8_t CURRENT_SCHEMA_VERSION = 2; // Current EEPROM schema version
} // namespace EEPROM_SIZES

class EEPROM_Manager {
private:
  bool isInitialized;
  bool isDevMode;
  bool _dirty;
  uint32_t _lastFlushMs;
  static constexpr uint32_t EEPROM_FLUSH_INTERVAL_MS = 5000;

  // Private constructor for singleton pattern
  EEPROM_Manager();

  // Helper methods
  bool ensureInitialized();
  void logOperation(const char* operation, const char* details = nullptr);
  bool beginEEPROM();
  void handleInitFailure();
  void markDirty();

public:
  // Singleton pattern
  static EEPROM_Manager& getInstance();

  // Delete copy and move to enforce singleton
  EEPROM_Manager(const EEPROM_Manager&) = delete;
  EEPROM_Manager& operator=(const EEPROM_Manager&) = delete;
  EEPROM_Manager(EEPROM_Manager&&) = delete;
  EEPROM_Manager& operator=(EEPROM_Manager&&) = delete;

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
  bool loadKnownMasterMac(uint8_t mac[6]);
  void saveKnownMasterMac(const uint8_t mac[6]);
  void clearKnownMasterMac();

  // TX power preset — deployment-specific, persisted across reboots
  planetopia::config::TxPowerPreset loadTxPowerPreset();
  void saveTxPowerPreset(planetopia::config::TxPowerPreset preset);

  // Node ID — logical node ID assigned by server (0 = unset)
  uint8_t loadNodeId();
  void saveNodeId(uint8_t nodeId);

  // Deferred flush API
  void flushIfDirty();
  void forceFlush();

  // Utility operations
  void clearAll();
  void clearRange(uint16_t startAddr, uint16_t endAddr);
  bool isAddressValid(uint16_t address);

  // Debug and diagnostics
  void dumpEEPROM();
  void printAddress(uint16_t address, uint16_t length);

  // Destructor
  ~EEPROM_Manager();
};

} // namespace utils
} // namespace planetopia

#endif // EEPROM_MANAGER_H
