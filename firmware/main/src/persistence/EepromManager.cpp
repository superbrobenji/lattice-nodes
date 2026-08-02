#include "EepromManager.h"
#include "src/error/Error.h"

namespace lattice {
namespace utils {

EepromManager::EepromManager() : isInitialized(false), isDevMode(false) {}

EepromManager::~EepromManager() {
  if (isInitialized) {
    _prefs.end();
  }
}

EepromManager& EepromManager::getInstance() {
  static EepromManager instance;
  return instance;
}

bool EepromManager::init() {
  if (isInitialized)
    return true;

  if (!_prefs.begin("lattice", false)) {
    Logger::logln("EEPROM", "Failed to initialize NVS", LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromManager: NVS begin failed");
    return false;
  }

  isInitialized = true;
  logOperation("Initialized", "NVS ready");
  return true;
}

void EepromManager::setDevMode(bool devMode) {
  isDevMode = devMode;
  logOperation("Dev mode set", devMode ? "Development mode enabled" : "Production mode enabled");
}

bool EepromManager::getDevMode() const {
  return isDevMode;
}

bool EepromManager::ensureInitialized() {
  if (!isInitialized) {
    Logger::logln("EEPROM", "NVS not initialized", LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void EepromManager::logOperation(const char* operation, const char* details) {
  if (details) {
    Logger::logln("EEPROM", String(operation) + ": " + details, LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("EEPROM", operation, LogLevel::LOG_DEBUG);
  }
}

// Deferred flush API — no-ops for NVS (commits are immediate)
void EepromManager::flushIfDirty() {
  // No-op
}

void EepromManager::forceFlush() {
  // No-op
}

// Master flag operations
bool EepromManager::loadMasterFlag() {
  if (!ensureInitialized())
    return false;

  bool isMaster = _prefs.getBool(NVS_KEYS::MASTER_FLAG, false);
  logOperation("Master flag loaded", isMaster ? "Master" : "Node");
  return isMaster;
}

void EepromManager::saveMasterFlag(bool isMaster) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Master flag save skipped", "Dev mode - no NVS storage");
    return;
  }

  _prefs.putBool(NVS_KEYS::MASTER_FLAG, isMaster);
  logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

// Dev flag operations
bool EepromManager::loadDevFlag() {
  if (!ensureInitialized())
    return false;

  bool isDev = _prefs.getBool(NVS_KEYS::DEV_FLAG, false);
  logOperation("Dev flag loaded", isDev ? "Development" : "Production");
  return isDev;
}

void EepromManager::saveDevFlag(bool isDev) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Dev flag save skipped", "Dev mode - no NVS storage");
    return;
  }

  _prefs.putBool(NVS_KEYS::DEV_FLAG, isDev);
  logOperation("Dev flag saved", isDev ? "Development" : "Production");
}

// Mesh key operations
bool EepromManager::loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized())
    return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) {
    Logger::logln("EEPROM", "Invalid key size for mesh key", LogLevel::LOG_ERROR);
    return false;
  }

  size_t len = _prefs.getBytes(NVS_KEYS::MESH_KEY, key, keySize);
  if (len != keySize) {
    // Key not found or wrong size
    return false;
  }
  logOperation("Mesh key loaded");
  return true;
}

void EepromManager::saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!ensureInitialized())
    return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE) {
    Logger::logln("EEPROM", "Invalid key size for mesh key", LogLevel::LOG_ERROR);
    return;
  }
  if (isDevMode) {
    logOperation("Mesh key save skipped", "Dev mode - no NVS storage");
    return;
  }

  _prefs.putBytes(NVS_KEYS::MESH_KEY, key, keySize);
  logOperation("Mesh key saved");
}

// Peer list operations
bool EepromManager::loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  lattice::err::check(peerRecords != nullptr, lattice::utils::ErrorType::CONFIG_ERROR,
                      "loadPeerList: peerRecords null");
  if (!ensureInitialized())
    return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Requested peer count exceeds maximum", LogLevel::LOG_ERROR);
    return false;
  }

  size_t expectedLen = maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  size_t len = _prefs.getBytes(NVS_KEYS::PEER_LIST, peerRecords, expectedLen);
  
  if (len == 0) {
    // No peers stored — fill with 0xFF and return false
    memset(peerRecords, 0xFF, expectedLen);
    logOperation("Peer list loaded", "No peers found");
    return false;
  }

  logOperation("Peer list loaded", String(maxPeers).c_str());
  return true;
}

void EepromManager::savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  lattice::err::check(peerRecords != nullptr, lattice::utils::ErrorType::CONFIG_ERROR,
                      "savePeerList: peerRecords null");
  if (!ensureInitialized())
    return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Peer count exceeds maximum", LogLevel::LOG_ERROR);
    return;
  }
  if (isDevMode) {
    logOperation("Peer list save skipped", "Dev mode - no NVS storage");
    return;
  }

  size_t len = numPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  if (numPeers == 0) {
    // Clear peer list
    _prefs.remove(NVS_KEYS::PEER_LIST);
  } else {
    _prefs.putBytes(NVS_KEYS::PEER_LIST, peerRecords, len);
  }
  logOperation("Peer list saved", String(numPeers).c_str());
}

bool EepromManager::hasPeers() {
  if (!ensureInitialized())
    return false;

  return _prefs.isKey(NVS_KEYS::PEER_LIST);
}

void EepromManager::clearPeerList() {
  if (!ensureInitialized())
    return;

  _prefs.remove(NVS_KEYS::PEER_LIST);
  logOperation("Peer list cleared");
}

// Adapter type operations
uint8_t EepromManager::loadAdapterType() {
  if (!ensureInitialized())
    return 0xFF;

  uint8_t adapterType = _prefs.getUChar(NVS_KEYS::ADAPTER_TYPE, 0);
  logOperation("Adapter type loaded", String(adapterType).c_str());
  return adapterType;
}

void EepromManager::saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Adapter type save skipped", "Dev mode - no NVS storage");
    return;
  }

  _prefs.putUChar(NVS_KEYS::ADAPTER_TYPE, adapterType);
  logOperation("Adapter type saved", String(adapterType).c_str());
}

// Reboot tracking operations
uint8_t EepromManager::loadRebootCount() {
  if (!ensureInitialized())
    return 0;
  return _prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
}

void EepromManager::saveRebootCount(uint8_t count) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putUChar(NVS_KEYS::REBOOT_COUNT, count);
}

void EepromManager::saveRebootReason(uint8_t reason) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putUChar(NVS_KEYS::REBOOT_REASON, reason);
}

uint8_t EepromManager::loadRebootReason() {
  if (!ensureInitialized())
    return 0xFF;
  return _prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
}

// CRC16 (CCITT) over a byte buffer
static uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// Keypair operations
bool EepromManager::loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!ensureInitialized())
    return false;

  size_t privLen = _prefs.getBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubLen = _prefs.getBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);

  if (privLen != 32 || pubLen != 32) {
    Logger::logln("EEPROM", "Keypair not found or incomplete", LogLevel::LOG_WARN);
    return false;
  }

  uint16_t stored = _prefs.getUInt(NVS_KEYS::KEYPAIR_CRC, 0);
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = crc16(both, 64);

  if (stored != computed) {
    Logger::logln("EEPROM", "Keypair CRC mismatch — keys unset or corrupted", LogLevel::LOG_WARN);
    return false;
  }

  return true;
}

void EepromManager::saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!ensureInitialized() || isDevMode)
    return;

  _prefs.putBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  _prefs.putBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);

  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = crc16(both, 64);
  _prefs.putUInt(NVS_KEYS::KEYPAIR_CRC, crc);

  logOperation("Keypair saved");
}

bool EepromManager::loadEnrolledFlag() {
  if (!ensureInitialized())
    return false;
  return _prefs.getBool(NVS_KEYS::ENROLLED_FLAG, false);
}

void EepromManager::saveEnrolledFlag(bool enrolled) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putBool(NVS_KEYS::ENROLLED_FLAG, enrolled);
}

uint32_t EepromManager::loadBootEpoch() {
  if (!ensureInitialized())
    return 0;
  return _prefs.getUInt(NVS_KEYS::BOOT_EPOCH, 0);
}

void EepromManager::saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch);
}

// TOFU master MAC operations
bool EepromManager::loadKnownMasterMac(uint8_t* mac) {
  if (!ensureInitialized())
    return false;

  size_t len = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  if (len != 6) {
    // Not found
    return false;
  }

  // Check if all 0xFF (unset sentinel)
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void EepromManager::saveKnownMasterMac(const uint8_t* mac) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  logOperation("Known master MAC saved");
}

void EepromManager::clearKnownMasterMac() {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC);
  logOperation("Known master MAC cleared");
}

// TOFU secondary master MAC operations
bool EepromManager::loadKnownMasterMacSecondary(uint8_t* mac) {
  if (!ensureInitialized())
    return false;

  size_t len = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC_SECONDARY, mac, 6);
  if (len != 6) {
    // Not found
    return false;
  }

  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void EepromManager::saveKnownMasterMacSecondary(const uint8_t* mac) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC_SECONDARY, mac, 6);
  logOperation("Known secondary master MAC saved");
}

void EepromManager::clearKnownMasterMacSecondary() {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC_SECONDARY);
}

// TX power preset operations
lattice::config::TxPowerPreset EepromManager::loadTxPowerPreset() {
  if (!ensureInitialized())
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  
  uint8_t val = _prefs.getUChar(NVS_KEYS::TX_POWER_PRESET, static_cast<uint8_t>(lattice::config::DEFAULT_TX_POWER_PRESET));
  if (val > 2)
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  return static_cast<lattice::config::TxPowerPreset>(val);
}

void EepromManager::saveTxPowerPreset(lattice::config::TxPowerPreset preset) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putUChar(NVS_KEYS::TX_POWER_PRESET, static_cast<uint8_t>(preset));
  logOperation("TX power preset saved");
}

// Node ID operations
void EepromManager::saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized())
    return;
  _prefs.putUChar(NVS_KEYS::NODE_ID, nodeId);
  logOperation("saveNodeId");
}

uint8_t EepromManager::loadNodeId() {
  if (!ensureInitialized())
    return 0;
  return _prefs.getUChar(NVS_KEYS::NODE_ID, 0);
}

// Utility operations
void EepromManager::clearAll() {
  if (!ensureInitialized())
    return;

  _prefs.clear();
  logOperation("All NVS cleared");
}

// Debug and diagnostics
void EepromManager::dumpEEPROM() {
  if (!ensureInitialized())
    return;

  Logger::logln("EEPROM", "=== NVS Dump (key-value store) ===", LogLevel::LOG_INFO);
  // NVS doesn't support iteration in the Preferences API, so this is a no-op
  // or could be extended to dump known keys
}

} // namespace utils
} // namespace lattice
