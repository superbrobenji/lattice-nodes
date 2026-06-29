#include "EEPROM_Manager.h"
#include "src/error/Error.h"

namespace planetopia {
namespace utils {

EEPROM_Manager::EEPROM_Manager()
    : isInitialized(false), isDevMode(false), _dirty(false), _lastFlushMs(0) {}

EEPROM_Manager::~EEPROM_Manager() {
  if (isInitialized) {
    EEPROM.end();
  }
}

EEPROM_Manager& EEPROM_Manager::getInstance() {
  static EEPROM_Manager instance;
  return instance;
}

// Tiger Style helpers
bool EEPROM_Manager::beginEEPROM() {
  if (EEPROM.begin(EEPROM_SIZES::TOTAL_SIZE))
    return true;
  handleInitFailure();
  return false;
}

void EEPROM_Manager::handleInitFailure() {
  Logger::logln("EEPROM", "Failed to initialize EEPROM", LogLevel::LOG_ERROR);
  planetopia::err::fail(planetopia::core::ErrorTypeDigit::MEMORY,
                        planetopia::core::ModuleDigit::EEPROM, 1,
                        "EEPROM_Manager: EEPROM.begin failed");
}

// --- refactored init with formal schema versioning ---
bool EEPROM_Manager::init() {
  if (isInitialized)
    return true;
  if (!beginEEPROM())
    return false;
  isInitialized = true;

  // Version check: read schema version byte and migrate if needed
  uint8_t storedVersion = EEPROM.read(EEPROM_ADDRESSES::SCHEMA_VERSION);

  if (storedVersion == 0xFF) {
    // Blank EEPROM — write current version and proceed
    Logger::logln("EEPROM", "Fresh EEPROM — version 2 written", LogLevel::LOG_INFO);
    EEPROM.write(EEPROM_ADDRESSES::SCHEMA_VERSION, EEPROM_SIZES::CURRENT_SCHEMA_VERSION);
    EEPROM.commit();
  } else if (storedVersion < EEPROM_SIZES::CURRENT_SCHEMA_VERSION) {
    // Version mismatch: run versioned migration handlers
    Logger::logln("EEPROM",
                  String("EEPROM version mismatch: stored=") + storedVersion +
                      " expected=" + EEPROM_SIZES::CURRENT_SCHEMA_VERSION + " — running migration",
                  LogLevel::LOG_WARN);

    if (storedVersion == 0x00) {
      // v1→v2 migration
      // Detect if old addresses still hold valid data (v1 layout used TOTAL_SIZE=256).
      // Migration strategy:
      //   1. Copy reboot tracking bytes from old addresses (92, 93) to new (412, 413).
      //   2. Copy keypair + CRC + enrolled flag from old addresses to new ones.
      //   3. Wipe old addresses so they don't interfere.
      //   4. Clear peer list (old records were 6-byte MAC only; new format is 38 bytes).
      //      Peers will be re-added at runtime via discovery/re-enrollment.
      Logger::logln("EEPROM", "v1→v2 layout migration running...", LogLevel::LOG_INFO);

      // Migrate reboot tracking
      uint8_t oldReason = EEPROM.read(EEPROM_ADDRESSES::V1_REBOOT_REASON);
      uint8_t oldCount = EEPROM.read(EEPROM_ADDRESSES::V1_REBOOT_COUNT);
      EEPROM.write(EEPROM_ADDRESSES::REBOOT_REASON, (oldReason == 0x00) ? 0xFF : oldReason);
      EEPROM.write(EEPROM_ADDRESSES::REBOOT_COUNT, (oldCount > 10) ? 0 : oldCount);

      // Migrate keypair (private key, public key, CRC, enrolled flag)
      for (int i = 0; i < 32; ++i) {
        EEPROM.write(EEPROM_ADDRESSES::PRIVATE_KEY + i,
                     EEPROM.read(EEPROM_ADDRESSES::V1_PRIVATE_KEY + i));
        EEPROM.write(EEPROM_ADDRESSES::PUBLIC_KEY + i,
                     EEPROM.read(EEPROM_ADDRESSES::V1_PUBLIC_KEY + i));
      }
      EEPROM.write(EEPROM_ADDRESSES::KEYPAIR_CRC, EEPROM.read(EEPROM_ADDRESSES::V1_KEYPAIR_CRC));
      EEPROM.write(EEPROM_ADDRESSES::KEYPAIR_CRC + 1,
                   EEPROM.read(EEPROM_ADDRESSES::V1_KEYPAIR_CRC + 1));
      EEPROM.write(EEPROM_ADDRESSES::ENROLLED_FLAG,
                   EEPROM.read(EEPROM_ADDRESSES::V1_ENROLLED_FLAG));

      // Wipe old addresses (now overlapped by new PEER_LIST range 32..411)
      // Only wipe 92..163 to avoid touching MESH_KEY (16..31) and MASTER/DEV flags (0,1).
      for (uint16_t addr = 92; addr <= 163; ++addr) {
        EEPROM.write(addr, 0xFF);
      }

      // Wipe full peer list region so stale 6-byte MAC records don't pollute 38-byte reads
      for (uint16_t i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
        EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, 0xFF);
      }

      Logger::logln("EEPROM", "v1→v2 migration complete", LogLevel::LOG_INFO);
    }
    // Future: add migration handlers for v2→v3, v3→v4, etc. here

    // Write updated version and commit
    EEPROM.write(EEPROM_ADDRESSES::SCHEMA_VERSION, EEPROM_SIZES::CURRENT_SCHEMA_VERSION);
    EEPROM.commit();
  } else if (storedVersion > EEPROM_SIZES::CURRENT_SCHEMA_VERSION) {
    // Firmware downgrade: EEPROM is from a newer firmware version
    Logger::logln("EEPROM",
                  String("EEPROM version mismatch: stored=") + storedVersion +
                      " expected=" + EEPROM_SIZES::CURRENT_SCHEMA_VERSION +
                      " — clearing EEPROM to recover from firmware downgrade",
                  LogLevel::LOG_WARN);
    clearAll();
    EEPROM.write(EEPROM_ADDRESSES::SCHEMA_VERSION, EEPROM_SIZES::CURRENT_SCHEMA_VERSION);
    EEPROM.commit();
  }
  // else: storedVersion == CURRENT_SCHEMA_VERSION, no migration needed

  // Normal boot: validate WDT tracking bytes
  if (EEPROM.read(EEPROM_ADDRESSES::REBOOT_REASON) == 0x00) {
    EEPROM.write(EEPROM_ADDRESSES::REBOOT_REASON, 0xFF);
  }
  if (EEPROM.read(EEPROM_ADDRESSES::REBOOT_COUNT) > 10) {
    EEPROM.write(EEPROM_ADDRESSES::REBOOT_COUNT, 0);
  }
  EEPROM.commit();

  logOperation("Initialized", "EEPROM ready");
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

// Deferred flush implementation
void EEPROM_Manager::markDirty() {
  _dirty = true;
}

void EEPROM_Manager::flushIfDirty() {
  if (!_dirty)
    return;
  if (millis() - _lastFlushMs < EEPROM_FLUSH_INTERVAL_MS)
    return;
  EEPROM.commit();
  _dirty = false;
  _lastFlushMs = millis();
  logOperation("EEPROM flushed (deferred)");
}

void EEPROM_Manager::forceFlush() {
  if (!_dirty)
    return;
  EEPROM.commit();
  _dirty = false;
  _lastFlushMs = millis();
}

// Master flag operations
bool EEPROM_Manager::loadMasterFlag() {
  if (!ensureInitialized())
    return false;

  uint8_t flag = EEPROM.read(EEPROM_ADDRESSES::MASTER_FLAG);
  bool isMaster = (flag == 1);
  logOperation("Master flag loaded", isMaster ? "Master" : "Node");
  return isMaster;
}

void EEPROM_Manager::saveMasterFlag(bool isMaster) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Master flag save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::MASTER_FLAG, isMaster ? 1 : 0);
  markDirty();
  logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

// Dev flag operations
bool EEPROM_Manager::loadDevFlag() {
  if (!ensureInitialized())
    return false;

  uint8_t flag = EEPROM.read(EEPROM_ADDRESSES::DEV_FLAG);
  bool isDev = (flag == 1);
  logOperation("Dev flag loaded", isDev ? "Development" : "Production");
  return isDev;
}

void EEPROM_Manager::saveDevFlag(bool isDev) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Dev flag save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::DEV_FLAG, isDev ? 1 : 0);
  markDirty();
  logOperation("Dev flag saved", isDev ? "Development" : "Production");
}

// Mesh key operations
bool EEPROM_Manager::loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized())
    return false;
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
  if (!ensureInitialized())
    return;
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
  markDirty();
  logOperation("Mesh key saved");
}

// Peer list operations
// Each peer record is PEER_RECORD_SIZE (38) bytes: 6-byte MAC + 32-byte Curve25519 public key.
bool EEPROM_Manager::loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  planetopia::err::check(peerRecords != nullptr, planetopia::utils::ErrorType::CONFIG_ERROR,
                         "loadPeerList: peerRecords null");
  if (!ensureInitialized())
    return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Requested peer count exceeds maximum", LogLevel::LOG_ERROR);
    return false;
  }

  for (int i = 0; i < static_cast<int>(maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE); ++i) {
    peerRecords[i] = EEPROM.read(EEPROM_ADDRESSES::PEER_LIST + i);
  }
  logOperation("Peer list loaded", String(maxPeers).c_str());
  return true;
}

void EEPROM_Manager::savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  planetopia::err::check(peerRecords != nullptr, planetopia::utils::ErrorType::CONFIG_ERROR,
                         "savePeerList: peerRecords null");
  if (!ensureInitialized())
    return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS) {
    Logger::logln("EEPROM", "Peer count exceeds maximum", LogLevel::LOG_ERROR);
    return;
  }
  if (isDevMode) {
    logOperation("Peer list save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  // Write all-0xFF first to blank slots, then new data, single commit at the end.
  // Never commit between clear and write — power loss would erase the list permanently.
  for (int i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, 0xFF);
  }
  for (int i = 0; i < static_cast<int>(numPeers * EEPROM_SIZES::PEER_RECORD_SIZE); ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, peerRecords[i]);
  }
  markDirty(); // Deferred commit — periodic flush coalesces burst writes
  logOperation("Peer list saved", String(numPeers).c_str());
}

bool EEPROM_Manager::hasPeers() {
  if (!ensureInitialized())
    return false;

  for (int i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
    if (EEPROM.read(EEPROM_ADDRESSES::PEER_LIST + i) != 0xFF) {
      return true;
    }
  }
  return false;
}

void EEPROM_Manager::clearPeerList() {
  if (!ensureInitialized())
    return;

  for (int i = 0; i < EEPROM_SIZES::PEER_LIST_SIZE; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PEER_LIST + i, 0xFF);
  }
  markDirty();
  logOperation("Peer list cleared");
}

// Adapter type operations
uint8_t EEPROM_Manager::loadAdapterType() {
  if (!ensureInitialized())
    return 0xFF;

  uint8_t adapterType = EEPROM.read(EEPROM_ADDRESSES::ADAPTER_TYPE);
  if (adapterType == 0xFF) {
    adapterType = 0; // Default to PIR adapter
  }
  logOperation("Adapter type loaded", String(adapterType).c_str());
  return adapterType;
}

void EEPROM_Manager::saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    logOperation("Adapter type save skipped", "Dev mode - no EEPROM storage");
    return;
  }

  EEPROM.write(EEPROM_ADDRESSES::ADAPTER_TYPE, adapterType);
  markDirty();
  logOperation("Adapter type saved", String(adapterType).c_str());
}

// Reboot tracking operations
uint8_t EEPROM_Manager::loadRebootCount() {
  if (!ensureInitialized())
    return 0;
  return EEPROM.read(EEPROM_ADDRESSES::REBOOT_COUNT);
}
void EEPROM_Manager::saveRebootCount(uint8_t count) {
  if (!ensureInitialized() || isDevMode)
    return;
  EEPROM.write(EEPROM_ADDRESSES::REBOOT_COUNT, count);
  markDirty();
}
void EEPROM_Manager::saveRebootReason(uint8_t reason) {
  if (!ensureInitialized() || isDevMode)
    return;
  EEPROM.write(EEPROM_ADDRESSES::REBOOT_REASON, reason);
  markDirty();
}
uint8_t EEPROM_Manager::loadRebootReason() {
  if (!ensureInitialized())
    return 0xFF;
  return EEPROM.read(EEPROM_ADDRESSES::REBOOT_REASON);
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
bool EEPROM_Manager::loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!ensureInitialized())
    return false;
  for (int i = 0; i < 32; ++i) {
    privateKey32[i] = EEPROM.read(EEPROM_ADDRESSES::PRIVATE_KEY + i);
    publicKey32[i] = EEPROM.read(EEPROM_ADDRESSES::PUBLIC_KEY + i);
  }
  uint16_t stored = static_cast<uint16_t>(EEPROM.read(EEPROM_ADDRESSES::KEYPAIR_CRC)) |
                    (static_cast<uint16_t>(EEPROM.read(EEPROM_ADDRESSES::KEYPAIR_CRC + 1)) << 8);
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

void EEPROM_Manager::saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 32; ++i) {
    EEPROM.write(EEPROM_ADDRESSES::PRIVATE_KEY + i, privateKey32[i]);
    EEPROM.write(EEPROM_ADDRESSES::PUBLIC_KEY + i, publicKey32[i]);
  }
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = crc16(both, 64);
  EEPROM.write(EEPROM_ADDRESSES::KEYPAIR_CRC, static_cast<uint8_t>(crc & 0xFF));
  EEPROM.write(EEPROM_ADDRESSES::KEYPAIR_CRC + 1, static_cast<uint8_t>((crc >> 8) & 0xFF));
  EEPROM.commit();
  logOperation("Keypair saved");
}

bool EEPROM_Manager::loadEnrolledFlag() {
  if (!ensureInitialized())
    return false;
  return EEPROM.read(EEPROM_ADDRESSES::ENROLLED_FLAG) == 0x01;
}

void EEPROM_Manager::saveEnrolledFlag(bool enrolled) {
  if (!ensureInitialized() || isDevMode)
    return;
  EEPROM.write(EEPROM_ADDRESSES::ENROLLED_FLAG, enrolled ? 0x01 : 0xFF);
  EEPROM.commit();
}

uint32_t EEPROM_Manager::loadBootEpoch() {
  if (!ensureInitialized())
    return 0;
  uint32_t epoch = 0;
  for (int i = 0; i < 4; ++i)
    epoch |= static_cast<uint32_t>(EEPROM.read(EEPROM_ADDRESSES::BOOT_EPOCH + i)) << (i * 8);
  return (epoch == 0xFFFFFFFF) ? 0 : epoch;
}

void EEPROM_Manager::saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 4; ++i)
    EEPROM.write(EEPROM_ADDRESSES::BOOT_EPOCH + i, static_cast<uint8_t>((epoch >> (i * 8)) & 0xFF));
  EEPROM.commit();
}

// TOFU master MAC operations
bool EEPROM_Manager::loadKnownMasterMac(uint8_t mac[6]) {
  if (!ensureInitialized())
    return false;
  for (int i = 0; i < 6; ++i)
    mac[i] = EEPROM.read(EEPROM_ADDRESSES::KNOWN_MASTER_MAC + i);
  // All 0xFF means unset (factory state)
  bool allFF = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) {
      allFF = false;
      break;
    }
  }
  return !allFF;
}

void EEPROM_Manager::saveKnownMasterMac(const uint8_t mac[6]) {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 6; ++i)
    EEPROM.write(EEPROM_ADDRESSES::KNOWN_MASTER_MAC + i, mac[i]);
  EEPROM.commit(); // Immediate commit — TOFU security anchor must survive power loss
  logOperation("Known master MAC saved");
}

void EEPROM_Manager::clearKnownMasterMac() {
  if (!ensureInitialized() || isDevMode)
    return;
  for (int i = 0; i < 6; ++i)
    EEPROM.write(EEPROM_ADDRESSES::KNOWN_MASTER_MAC + i, 0xFF);
  EEPROM.commit(); // Immediate commit — TOFU security anchor must survive power loss
  logOperation("Known master MAC cleared");
}

// TX power preset operations
planetopia::config::TxPowerPreset EEPROM_Manager::loadTxPowerPreset() {
  if (!ensureInitialized())
    return planetopia::config::DEFAULT_TX_POWER_PRESET;
  uint8_t val = EEPROM.read(EEPROM_ADDRESSES::TX_POWER_PRESET);
  if (val > 2 || val == 0xFF)
    return planetopia::config::DEFAULT_TX_POWER_PRESET;
  return static_cast<planetopia::config::TxPowerPreset>(val);
}

void EEPROM_Manager::saveTxPowerPreset(planetopia::config::TxPowerPreset preset) {
  if (!ensureInitialized() || isDevMode)
    return;
  EEPROM.write(EEPROM_ADDRESSES::TX_POWER_PRESET, static_cast<uint8_t>(preset));
  EEPROM.commit(); // Immediate commit — deployment config must survive reboot
  logOperation("TX power preset saved");
}

// Node ID operations
void EEPROM_Manager::saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized())
    return;
  EEPROM.write(EEPROM_ADDRESSES::NODE_ID, nodeId);
  markDirty();
  logOperation("saveNodeId");
}

uint8_t EEPROM_Manager::loadNodeId() {
  if (!ensureInitialized())
    return 0;
  uint8_t raw = EEPROM.read(EEPROM_ADDRESSES::NODE_ID);
  return (raw == 0xFF) ? 0 : raw;
}

// Utility operations
void EEPROM_Manager::clearAll() {
  if (!ensureInitialized())
    return;

  for (int i = 0; i < EEPROM_SIZES::TOTAL_SIZE; ++i) {
    EEPROM.write(static_cast<uint16_t>(i), 0xFF);
  }
  EEPROM.commit();
  logOperation("All EEPROM cleared");
}

void EEPROM_Manager::clearRange(uint16_t startAddr, uint16_t endAddr) {
  if (!ensureInitialized())
    return;
  if (!isAddressValid(startAddr) || !isAddressValid(endAddr) || startAddr > endAddr) {
    Logger::logln("EEPROM", "Invalid address range for clear", LogLevel::LOG_ERROR);
    return;
  }

  for (uint16_t i = startAddr; i <= endAddr; ++i) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();
  String msg = String(startAddr) + " to " + String(endAddr);
  logOperation("EEPROM range cleared", msg.c_str());
}

bool EEPROM_Manager::isAddressValid(uint16_t address) {
  return address < EEPROM_SIZES::TOTAL_SIZE;
}

// Debug and diagnostics
void EEPROM_Manager::dumpEEPROM() {
  if (!ensureInitialized())
    return;

  Logger::logln("EEPROM", "=== EEPROM Dump ===", LogLevel::LOG_INFO);
  for (uint16_t i = 0; i < EEPROM_SIZES::TOTAL_SIZE; i += 16) {
    String line = String(i, HEX) + ": ";
    for (uint8_t j = 0; j < 16 && (i + j) < EEPROM_SIZES::TOTAL_SIZE; ++j) {
      uint16_t addr = static_cast<uint16_t>(i + j);
      // Skip keypair range — private key must never appear in debug output
      if (addr >= EEPROM_ADDRESSES::PRIVATE_KEY && addr <= EEPROM_ADDRESSES::KEYPAIR_CRC + 1) {
        line += "XX ";
        continue;
      }
      uint8_t val = EEPROM.read(addr);
      if (val < 16)
        line += "0";
      line += String(val, HEX) + " ";
    }
    Logger::logln("EEPROM", line, LogLevel::LOG_DEBUG);
  }
}

void EEPROM_Manager::printAddress(uint16_t address, uint16_t length) {
  if (!ensureInitialized())
    return;
  if (!isAddressValid(address) || !isAddressValid(address + length - 1)) {
    Logger::logln("EEPROM", "Invalid address range for print", LogLevel::LOG_ERROR);
    return;
  }

  String line = "Addr " + String(address) + ": ";
  for (uint16_t i = 0; i < length; ++i) {
    uint8_t val = EEPROM.read(static_cast<uint16_t>(address + i));
    if (val < 16)
      line += "0";
    line += String(val, HEX) + " ";
  }
  Logger::logln("EEPROM", line, LogLevel::LOG_DEBUG);
}

} // namespace utils
} // namespace planetopia
