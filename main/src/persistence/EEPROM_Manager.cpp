#include "EEPROM_Manager.h"
#include "src/core/ErrorHandler.h"

namespace planetopia {
namespace utils {

// Static instance pointer
EEPROM_Manager* EEPROM_Manager::instance = nullptr;

EEPROM_Manager::EEPROM_Manager()
  : isInitialized(false), isDevMode(false) {
}

EEPROM_Manager::~EEPROM_Manager() {
  if (isInitialized) {
    EEPROM.end();
  }
}

EEPROM_Manager& EEPROM_Manager::getInstance() {
  if (instance == nullptr) {
    instance = new EEPROM_Manager();
  }
  return *instance;
}

bool EEPROM_Manager::init() {
  if (isInitialized) {
    return true;
  }

  if (!EEPROM.begin(EEPROM_SIZES::TOTAL_SIZE)) {
    Logger::logln("EEPROM", "Failed to initialize EEPROM", LogLevel::LOG_ERROR);
    ErrorHandler::getInstance().signalError(
      ErrorType::MEMORY_ERROR,
      "EEPROM_Manager: Failed to initialize EEPROM");
    return false;
  }

  isInitialized = true;
  logOperation("Initialized", "EEPROM initialized successfully");
  return true;
}

void EEPROM_Manager::setDevMode(bool devMode) {
  isDevMode = devMode;
  logOperation("Dev mode set", devMode ? "Development mode enabled" : "Production mode enabled");
}

bool EEPROM_Manager::getDevMode() const {
  return isDevMode;
}

bool EEPROM_Manager::ensureInitialized() {
  if (!isInitialized) {
    Logger::logln("EEPROM", "EEPROM not initialized", LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void EEPROM_Manager::logOperation(const char* operation, const char* details) {
  if (details) {
    Logger::logln("EEPROM", String(operation) + ": " + details, LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("EEPROM", operation, LogLevel::LOG_DEBUG);
  }
}

// Master flag operations
bool EEPROM_Manager::loadMasterFlag() {
  if (!ensureInitialized()) return false;

  uint8_t flag = EEPROM.read(EEPROM_ADDRESSES::MASTER_FLAG);
  bool isMaster = (flag == 1);
  logOperation("Master flag loaded", isMaster ? "Master" : "Node");
  return isMaster;
}

void EEPROM_Manager::saveMasterFlag(bool isMaster) {
  if (!ensureInitialized()) return;
  if (isDevMode) {
    logOperation("Master flag save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::MASTER_FLAG, isMaster ? 1 : 0);
  EEPROM.commit();
  logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

// Dev flag operations
bool EEPROM_Manager::loadDevFlag() {
  if (!ensureInitialized()) return false;

  uint8_t flag = EEPROM.read(EEPROM_ADDRESSES::DEV_FLAG);
  bool isDev = (flag == 1);
  logOperation("Dev flag loaded", isDev ? "Development" : "Production");
  return isDev;
}

void EEPROM_Manager::saveDevFlag(bool isDev) {
  if (!ensureInitialized()) return;
  if (isDevMode) {
    logOperation("Dev flag save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::DEV_FLAG, isDev ? 1 : 0);
  EEPROM.commit();
  logOperation("Dev flag saved", isDev ? "Development" : "Production");
}

// Mesh key operations
bool EEPROM_Manager::loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized()) return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) {
    Logger::logln("EEPROM", "Invalid key size for mesh key", LogLevel::LOG_ERROR);
    return false;
  }

  for (int i = 0; i < EEPROM_SIZES::MESH_KEY_SIZE; ++i) {
    key[i] = EEPROM.read(EEPROM_ADDRESSES::MESH_KEY + i);
  }
  logOperation("Mesh key loaded");
  return true;
}

void EEPROM_Manager::saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!ensureInitialized()) return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) {
    Logger::logln("EEPROM", "Invalid key size for mesh key", LogLevel::LOG_ERROR);
    return;
  }
  if (isDevMode) {
    logOperation("Mesh key save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  for (int i = 0; i < EEPROM_SIZES::MESH_KEY_SIZE; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::MESH_KEY + i, key[i]);
  }
  EEPROM.commit();
  logOperation("Mesh key saved");
}

// Peer list operations
bool EEPROM_Manager::loadPeerList(uint8_t* peerList, size_t maxPeers) {
  if (!ensureInitialized()) return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Requested peer count exceeds maximum", LogLevel::LOG_ERROR);
    return false;
  }

  for (int i = 0; i < maxPeers * EEPROM_SIZES::PEER_MAC_SIZE; ++i) {
    peerList[i] = EEPROM.read(EEPROM_ADDRESSES::PEER_LIST + i);
  }
  logOperation("Peer list loaded", String(maxPeers).c_str());
  return true;
}

void EEPROM_Manager::savePeerList(const uint8_t* peerList, size_t numPeers) {
  if (!ensureInitialized()) return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Peer count exceeds maximum", LogLevel::LOG_ERROR);
    return;
  }
  if (isDevMode) {
    logOperation("Peer list save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  // Clear existing peer list first
  clearPeerList();

  // Write new peer list
  for (int i = 0; i < numPeers * EEPROM_SIZES::PEER_MAC_SIZE; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, peerList[i]);
  }
  EEPROM.commit();
  logOperation("Peer list saved", String(numPeers).c_str());
}

bool EEPROM_Manager::hasPeers() {
  if (!ensureInitialized()) return false;

  for (int i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
    if (EEPROM.read(EEPROM_ADDRESSES::PEER_LIST + i) != 0xFF) {
      return true;
    }
  }
  return false;
}

void EEPROM_Manager::clearPeerList() {
  if (!ensureInitialized()) return;

  for (int i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, 0xFF);
  }
  EEPROM.commit();
  logOperation("Peer list cleared");
}

// Adapter type operations
uint8_t EEPROM_Manager::loadAdapterType() {
  if (!ensureInitialized()) return 0xFF;

  uint8_t adapterType = EEPROM.read(EEPROM_ADDRESSES::ADAPTER_TYPE);
  if (adapterType == 0xFF) {
    adapterType = 0;  // Default to PIR adapter
  }
  logOperation("Adapter type loaded", String(adapterType).c_str());
  return adapterType;
}

void EEPROM_Manager::saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized()) return;
  if (isDevMode) {
    logOperation("Adapter type save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::ADAPTER_TYPE, adapterType);
  EEPROM.commit();
  logOperation("Adapter type saved", String(adapterType).c_str());
}

// Utility operations
void EEPROM_Manager::clearAll() {
  if (!ensureInitialized()) return;

  for (int i = 0; i < EEPROM_SIZES::TOTAL_SIZE; ++i) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();
  logOperation("All EEPROM cleared");
}

void EEPROM_Manager::clearRange(int startAddr, int endAddr) {
  if (!ensureInitialized()) return;
  if (!isAddressValid(startAddr) || !isAddressValid(endAddr) || startAddr > endAddr) {
    Logger::logln("EEPROM", "Invalid address range for clear", LogLevel::LOG_ERROR);
    return;
  }

  for (int i = startAddr; i <= endAddr; ++i) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();
  String msg = String(startAddr) + " to " + String(endAddr);
  logOperation("EEPROM range cleared", msg.c_str());
}

bool EEPROM_Manager::isAddressValid(int address) {
  return address >= 0 && address < EEPROM_SIZES::TOTAL_SIZE;
}

// Debug and diagnostics
void EEPROM_Manager::dumpEEPROM() {
  if (!ensureInitialized()) return;

  Logger::logln("EEPROM", "=== EEPROM Dump ===", LogLevel::LOG_INFO);
  for (int i = 0; i < EEPROM_SIZES::TOTAL_SIZE; i += 16) {
    String line = String(i, HEX) + ": ";
    for (int j = 0; j < 16 && (i + j) < EEPROM_SIZES::TOTAL_SIZE; ++j) {
      uint8_t val = EEPROM.read(i + j);
      if (val < 16) line += "0";
      line += String(val, HEX) + " ";
    }
    Logger::logln("EEPROM", line, LogLevel::LOG_DEBUG);
  }
}

void EEPROM_Manager::printAddress(int address, int length) {
  if (!ensureInitialized()) return;
  if (!isAddressValid(address) || !isAddressValid(address + length - 1)) {
    Logger::logln("EEPROM", "Invalid address range for print", LogLevel::LOG_ERROR);
    return;
  }

  String line = "Addr " + String(address) + ": ";
  for (int i = 0; i < length; ++i) {
    uint8_t val = EEPROM.read(address + i);
    if (val < 16) line += "0";
    line += String(val, HEX) + " ";
  }
  Logger::logln("EEPROM", line, LogLevel::LOG_DEBUG);
}

}  // namespace utils
}  // namespace planetopia
