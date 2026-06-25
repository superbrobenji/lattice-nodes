#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <EEPROM.h>
#include "src/core/Logger.h"

namespace planetopia {
namespace utils {

// EEPROM address constants - centralized in one place
namespace EEPROM_ADDRESSES {
constexpr uint16_t MASTER_FLAG = 0;   // Master flag (1 byte)
constexpr uint16_t DEV_FLAG = 1;      // Dev mode flag (1 byte)
constexpr uint16_t MESH_KEY = 16;     // Mesh encryption key (16 bytes)
constexpr uint16_t PEER_LIST = 32;    // Peer MAC addresses (60 bytes)
constexpr uint16_t ADAPTER_TYPE = 8;  // Adapter type (1 byte)
constexpr uint16_t REBOOT_REASON = 92;  // 1 byte: last reset reason
constexpr uint16_t REBOOT_COUNT  = 93;  // 1 byte: consecutive unexpected reboot count
constexpr uint16_t RESERVED = 94;       // Reserved for future use
}

// EEPROM size constants
namespace EEPROM_SIZES {
constexpr uint16_t TOTAL_SIZE = 128;
constexpr uint8_t MESH_KEY_SIZE = 16;
constexpr uint8_t MAX_PEERS = 10;
constexpr uint8_t PEER_MAC_SIZE = 6;
constexpr uint16_t PEER_LIST_SIZE = MAX_PEERS * PEER_MAC_SIZE;
}

class EEPROM_Manager {
private:
  static EEPROM_Manager* instance;
  bool isInitialized;
  bool isDevMode;

  // Private constructor for singleton pattern
  EEPROM_Manager();

  // Helper methods
  bool ensureInitialized();
  void logOperation(const char* operation, const char* details = nullptr);
  bool beginEEPROM();
  void handleInitFailure();

public:
  // Singleton pattern
  static EEPROM_Manager& getInstance();

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

  // Peer list operations
  bool loadPeerList(uint8_t* peerList, size_t maxPeers);
  void savePeerList(const uint8_t* peerList, size_t numPeers);
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

}  // namespace utils
}  // namespace planetopia

#endif  // EEPROM_MANAGER_H
