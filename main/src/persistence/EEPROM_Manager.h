#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
#include <EEPROM.h>
#include "src/core/Logger.h"

namespace planetopia {
namespace utils {

// EEPROM address constants - centralized in one place
namespace EEPROM_ADDRESSES {
constexpr int MASTER_FLAG = 0;   // Master flag (1 byte)
constexpr int DEV_FLAG = 1;      // Dev mode flag (1 byte)
constexpr int MESH_KEY = 16;     // Mesh encryption key (16 bytes)
constexpr int PEER_LIST = 32;    // Peer MAC addresses (60 bytes = 10 peers * 6 bytes)
constexpr int ADAPTER_TYPE = 8;  // Adapter type (1 byte)
constexpr int RESERVED = 92;     // Reserved for future use
}

// EEPROM size constants
namespace EEPROM_SIZES {
constexpr int TOTAL_SIZE = 128;                            // Total EEPROM size
constexpr int MESH_KEY_SIZE = 16;                          // Mesh key size
constexpr int MAX_PEERS = 10;                              // Maximum number of peers
constexpr int PEER_MAC_SIZE = 6;                           // MAC address size
constexpr int PEER_LIST_SIZE = MAX_PEERS * PEER_MAC_SIZE;  // Total peer list size
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

  // Utility operations
  void clearAll();
  void clearRange(int startAddr, int endAddr);
  bool isAddressValid(int address);

  // Debug and diagnostics
  void dumpEEPROM();
  void printAddress(int address, int length);

  // Destructor
  ~EEPROM_Manager();
};

}  // namespace utils
}  // namespace planetopia

#endif  // EEPROM_MANAGER_H
