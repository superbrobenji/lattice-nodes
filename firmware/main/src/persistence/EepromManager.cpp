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
  if (!_prefs.begin(NVS_KEYS::NAMESPACE, false)) {
    Logger::logln("NVS", "Failed to open NVS namespace", LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromManager: NVS begin failed");
    return false;
  }
  isInitialized = true;

  uint8_t reason = _prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
  if (reason == 0x00) {
    _prefs.putUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
  }
  uint8_t count = _prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
  if (count > 10) {
    _prefs.putUChar(NVS_KEYS::REBOOT_COUNT, 0);
  }

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
    Logger::logln("NVS", "NVS not initialized", LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void EepromManager::logOperation(const char* operation, const char* details) {
  if (details) {
    Logger::logln("NVS", String(operation) + ": " + details, LogLevel::LOG_DEBUG);
  } else {
    Logger::logln("NVS", operation, LogLevel::LOG_DEBUG);
  }
}

bool EepromManager::loadMasterFlag() {
  if (!ensureInitialized())
    return false;
  return _prefs.getBool(NVS_KEYS::MASTER_FLAG, false);
}

void EepromManager::saveMasterFlag(bool isMaster) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putBool(NVS_KEYS::MASTER_FLAG, isMaster);
  logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

bool EepromManager::loadDevFlag() {
  if (!ensureInitialized())
    return false;
  return _prefs.getBool(NVS_KEYS::DEV_FLAG, false);
}

void EepromManager::saveDevFlag(bool isDev) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putBool(NVS_KEYS::DEV_FLAG, isDev);
}

bool EepromManager::loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized())
    return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return false;
  size_t read = _prefs.getBytes(NVS_KEYS::MESH_KEY, key, keySize);
  return read == keySize;
}

void EepromManager::saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!ensureInitialized() || isDevMode)
    return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return;
  _prefs.putBytes(NVS_KEYS::MESH_KEY, key, keySize);
  logOperation("Mesh key saved");
}

bool EepromManager::loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  if (!ensureInitialized())
    return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS)
    return false;
  size_t maxBytes = maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  size_t read = _prefs.getBytes(NVS_KEYS::PEER_LIST, peerRecords, maxBytes);
  if (read == 0) {
    memset(peerRecords, 0xFF, maxBytes);
    return false;
  }
  return true;
}

void EepromManager::savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  if (!ensureInitialized() || isDevMode)
    return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS)
    return;
  if (numPeers == 0) {
    _prefs.remove(NVS_KEYS::PEER_LIST);
  } else {
    _prefs.putBytes(NVS_KEYS::PEER_LIST, peerRecords,
                    numPeers * EEPROM_SIZES::PEER_RECORD_SIZE);
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

uint8_t EepromManager::loadAdapterType() {
  if (!ensureInitialized())
    return 0;
  return _prefs.getUChar(NVS_KEYS::ADAPTER_TYPE, 0);
}

void EepromManager::saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.putUChar(NVS_KEYS::ADAPTER_TYPE, adapterType);
}

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

static uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

bool EepromManager::loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!ensureInitialized())
    return false;
  size_t privRead = _prefs.getBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubRead = _prefs.getBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  if (privRead != 32 || pubRead != 32)
    return false;
  uint32_t stored = _prefs.getUInt(NVS_KEYS::KEYPAIR_CRC, 0xFFFFFFFF);
  if (stored == 0xFFFFFFFF)
    return false;
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = crc16(both, 64);
  if (static_cast<uint16_t>(stored) != computed) {
    Logger::logln("NVS", "Keypair CRC mismatch", LogLevel::LOG_WARN);
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
  _prefs.putUInt(NVS_KEYS::KEYPAIR_CRC, static_cast<uint32_t>(crc));
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

bool EepromManager::loadKnownMasterMac(uint8_t* mac) {
  if (!ensureInitialized())
    return false;
  size_t read = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  if (read != 6)
    return false;
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

bool EepromManager::loadKnownMasterMacSecondary(uint8_t* mac) {
  if (!ensureInitialized())
    return false;
  size_t read = _prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  if (read != 6)
    return false;
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
  _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  logOperation("Known secondary master MAC saved");
}

void EepromManager::clearKnownMasterMacSecondary() {
  if (!ensureInitialized() || isDevMode)
    return;
  _prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC_SEC);
}

lattice::config::TxPowerPreset EepromManager::loadTxPowerPreset() {
  if (!ensureInitialized())
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  uint8_t val = _prefs.getUChar(NVS_KEYS::TX_POWER_PRESET, 0xFF);
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

uint8_t EepromManager::loadNodeId() {
  if (!ensureInitialized())
    return 0;
  return _prefs.getUChar(NVS_KEYS::NODE_ID, 0);
}

void EepromManager::saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized())
    return;
  _prefs.putUChar(NVS_KEYS::NODE_ID, nodeId);
  logOperation("saveNodeId");
}

void EepromManager::clearAll() {
  if (!ensureInitialized())
    return;
  _prefs.clear();
  logOperation("All NVS cleared");
}

void EepromManager::dumpEEPROM() {
  Logger::logln("NVS", "NVS dump not implemented (use idf.py nvs-dump)", LogLevel::LOG_INFO);
}

} // namespace utils
} // namespace lattice
