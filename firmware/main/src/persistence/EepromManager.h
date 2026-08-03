#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "src/logging/Logger.h"
#include "../../project_config.h"

namespace lattice {
namespace utils {

namespace NVS_KEYS {
constexpr const char* NAMESPACE = "lattice";
constexpr const char* MASTER_FLAG = "master";
constexpr const char* DEV_FLAG = "dev";
constexpr const char* ADAPTER_TYPE = "adapter";
constexpr const char* MESH_KEY = "meshkey";
constexpr const char* PEER_LIST = "peers";
constexpr const char* REBOOT_REASON = "rbt_reason";
constexpr const char* REBOOT_COUNT = "rbt_count";
constexpr const char* PRIVATE_KEY = "privkey";
constexpr const char* PUBLIC_KEY = "pubkey";
constexpr const char* KEYPAIR_CRC = "kp_crc";
constexpr const char* ENROLLED_FLAG = "enrolled";
constexpr const char* BOOT_EPOCH = "epoch";
constexpr const char* KNOWN_MASTER_MAC = "master_mac";
constexpr const char* KNOWN_MASTER_MAC_SEC = "master_mac2";
constexpr const char* TX_POWER_PRESET = "txpower";
constexpr const char* NODE_ID = "node_id";
} // namespace NVS_KEYS

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
  uint32_t _devEpoch = 0; // DEV_MODE RAM-only monotonic boot-epoch seed (issue #43)

  EepromManager();
  bool ensureInitialized();
  void logOperation(const char* operation, const char* details = nullptr);

  // Tiered NVS write-return handling (issue #43). `got`/`want` are the
  // bytes actually written vs. requested by the preceding put* call.
  // securityRelevant=true escalates a short write via lattice::err::fail
  // (halts the node); false logs at ERROR and lets the caller continue.
  bool _persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant);

public:
  static EepromManager& getInstance();

  EepromManager(const EepromManager&) = delete;
  EepromManager& operator=(const EepromManager&) = delete;
  EepromManager(EepromManager&&) = delete;
  EepromManager& operator=(EepromManager&&) = delete;

  bool init();
  void setDevMode(bool devMode);
  bool getDevMode() const;

  bool loadMasterFlag();
  void saveMasterFlag(bool isMaster);

  bool loadDevFlag();
  void saveDevFlag(bool isDev);

  bool loadMeshKey(uint8_t* key, size_t keySize);
  void saveMeshKey(const uint8_t* key, size_t keySize);

  bool loadPeerList(uint8_t* peerRecords, size_t maxPeers);
  void savePeerList(const uint8_t* peerRecords, size_t numPeers);
  bool hasPeers();
  void clearPeerList();

  uint8_t loadAdapterType();
  void saveAdapterType(uint8_t adapterType);

  uint8_t loadRebootCount();
  void saveRebootCount(uint8_t count);
  void saveRebootReason(uint8_t reason);
  uint8_t loadRebootReason();

  bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32);
  void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32);
  bool loadEnrolledFlag();
  void saveEnrolledFlag(bool enrolled);

  uint32_t loadBootEpoch();
  void saveBootEpoch(uint32_t epoch);

  bool loadKnownMasterMac(uint8_t* mac);
  void saveKnownMasterMac(const uint8_t* mac);
  void clearKnownMasterMac();

  bool loadKnownMasterMacSecondary(uint8_t* mac);
  void saveKnownMasterMacSecondary(const uint8_t* mac);
  void clearKnownMasterMacSecondary();

  lattice::config::TxPowerPreset loadTxPowerPreset();
  void saveTxPowerPreset(lattice::config::TxPowerPreset preset);

  uint8_t loadNodeId();
  void saveNodeId(uint8_t nodeId);

  void flushIfDirty() {}
  void forceFlush() {}

  void clearAll();
  void dumpEEPROM();

  ~EepromManager();

#ifdef UNIT_TEST
  bool isInitializedForTest() const { return isInitialized; }
#endif
};

} // namespace utils
} // namespace lattice

#endif // EEPROM_MANAGER_H
