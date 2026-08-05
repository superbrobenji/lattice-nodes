#include "EepromManager.h"
#include "src/error/Error.h"
#include <cstdio>

namespace lattice {
namespace eeprom {

using namespace lattice::utils;

namespace {

detail::State _state;

// Runs at process-exit (static-storage-duration teardown): closes NVS if it
// was ever opened.
struct Cleanup {
  ~Cleanup() {
    if (_state.isInitialized) {
      _state.prefs.end();
    }
  }
} _cleanup;

bool ensureInitialized() {
  if (!_state.isInitialized) {
    LATTICE_LOGLN("NVS", "NVS not initialized", lattice::utils::LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void logOperation(const char* operation, const char* details = nullptr) {
  if (details) {
    LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_DEBUG, "%s: %s", operation, details);
  } else {
    LATTICE_LOGLN("NVS", operation, lattice::utils::LogLevel::LOG_DEBUG);
  }
}

uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// Tiered NVS write-return handling (issue #43). `got`/`want` are the bytes
// actually written vs. requested by the preceding put* call. securityRelevant=true
// escalates a short write via lattice::err::fail (halts the node); false logs
// at ERROR and lets the caller continue.
bool persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant) {
  if (got == want) {
    return true;
  }
  if (securityRelevant) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 5,
                       "NVS write failed (security-relevant key)");
    return false; // unreachable outside UNIT_TEST
  }
  LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_ERROR, "write failed key=%s got=%u want=%u",
               key, (unsigned)got, (unsigned)want);
  return false;
}

} // namespace

bool init() {
  if (_state.isInitialized)
    return true;
  if (!_state.prefs.begin(NVS_KEYS::NAMESPACE, false)) {
    LATTICE_LOGLN("NVS", "Failed to open NVS namespace", lattice::utils::LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromManager: NVS begin failed");
    return false;
  }
  _state.isInitialized = true;

  uint8_t reason = _state.prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
  if (reason == 0x00) {
    size_t n = _state.prefs.putUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
    persistOrEscalate(NVS_KEYS::REBOOT_REASON, n, sizeof(uint8_t), /*securityRelevant=*/false);
  }
  uint8_t count = _state.prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
  if (count > 10) {
    size_t n = _state.prefs.putUChar(NVS_KEYS::REBOOT_COUNT, 0);
    persistOrEscalate(NVS_KEYS::REBOOT_COUNT, n, sizeof(uint8_t), /*securityRelevant=*/false);
  }

  logOperation("Initialized", "NVS ready");
  return true;
}

void setDevMode(bool devMode) {
  _state.isDevMode = devMode;
  logOperation("Dev mode set", devMode ? "Development mode enabled" : "Production mode enabled");
}

bool getDevMode() {
  return _state.isDevMode;
}

bool loadMasterFlag() {
  if (!ensureInitialized())
    return false;
  return _state.prefs.getBool(NVS_KEYS::MASTER_FLAG, false);
}

void saveMasterFlag(bool isMaster) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putBool(NVS_KEYS::MASTER_FLAG, isMaster);
  persistOrEscalate(NVS_KEYS::MASTER_FLAG, n, 1, /*securityRelevant=*/false);
  logOperation("Master flag saved", isMaster ? "Master" : "Node");
}

bool loadDevFlag() {
  if (!ensureInitialized())
    return false;
  return _state.prefs.getBool(NVS_KEYS::DEV_FLAG, false);
}

void saveDevFlag(bool isDev) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putBool(NVS_KEYS::DEV_FLAG, isDev);
  persistOrEscalate(NVS_KEYS::DEV_FLAG, n, 1, /*securityRelevant=*/false);
}

bool loadMeshKey(uint8_t* key, size_t keySize) {
  if (!ensureInitialized())
    return false;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return false;
  size_t read = _state.prefs.getBytes(NVS_KEYS::MESH_KEY, key, keySize);
  return read == keySize;
}

void saveMeshKey(const uint8_t* key, size_t keySize) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  if (keySize != EEPROM_SIZES::MESH_KEY_SIZE)
    return;
  size_t n = _state.prefs.putBytes(NVS_KEYS::MESH_KEY, key, keySize);
  persistOrEscalate(NVS_KEYS::MESH_KEY, n, keySize, /*securityRelevant=*/true);
  logOperation("Mesh key saved");
}

bool loadPeerList(uint8_t* peerRecords, size_t maxPeers) {
  if (!ensureInitialized())
    return false;
  if (maxPeers > EEPROM_SIZES::MAX_PEERS)
    return false;
  size_t maxBytes = maxPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
  // Pre-fill the whole output buffer with the "empty slot" sentinel (0xFF)
  // before reading: a persisted list shorter than maxBytes (fewer peers were
  // ever saved than MAX_PEERS) leaves getBytes() writing only the first
  // `read` bytes, and the caller (PeerRegistry::loadFromEEPROM) scans every
  // record up to maxPeers regardless of how many bytes came back. Without
  // this prefill, the untouched tail of the caller's uninitialized stack
  // buffer was read as real peer records — an uninitialized-memory bug that
  // could inject bogus peers with garbage MAC/public-key bytes.
  memset(peerRecords, 0xFF, maxBytes);
  size_t read = _state.prefs.getBytes(NVS_KEYS::PEER_LIST, peerRecords, maxBytes);
  if (read == 0) {
    return false;
  }
  return true;
}

void savePeerList(const uint8_t* peerRecords, size_t numPeers) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  if (numPeers > EEPROM_SIZES::MAX_PEERS)
    return;
  if (numPeers == 0) {
    _state.prefs.remove(NVS_KEYS::PEER_LIST);
  } else {
    size_t want = numPeers * EEPROM_SIZES::PEER_RECORD_SIZE;
    size_t n = _state.prefs.putBytes(NVS_KEYS::PEER_LIST, peerRecords, want);
    // securityRelevant=true: each record carries a peer's E2E public key —
    // trust material, same tier as MESH_KEY/KNOWN_MASTER_MAC below.
    persistOrEscalate(NVS_KEYS::PEER_LIST, n, want, /*securityRelevant=*/true);
  }
  logOperation("Peer list saved", String(numPeers).c_str());
}

bool hasPeers() {
  if (!ensureInitialized())
    return false;
  return _state.prefs.isKey(NVS_KEYS::PEER_LIST);
}

void clearPeerList() {
  if (!ensureInitialized())
    return;
  _state.prefs.remove(NVS_KEYS::PEER_LIST);
  logOperation("Peer list cleared");
}

uint8_t loadAdapterType() {
  if (!ensureInitialized())
    return 0;
  return _state.prefs.getUChar(NVS_KEYS::ADAPTER_TYPE, 0);
}

void saveAdapterType(uint8_t adapterType) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putUChar(NVS_KEYS::ADAPTER_TYPE, adapterType);
  persistOrEscalate(NVS_KEYS::ADAPTER_TYPE, n, sizeof(uint8_t), /*securityRelevant=*/false);
}

uint8_t loadRebootCount() {
  if (!ensureInitialized())
    return 0;
  return _state.prefs.getUChar(NVS_KEYS::REBOOT_COUNT, 0);
}

void saveRebootCount(uint8_t count) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putUChar(NVS_KEYS::REBOOT_COUNT, count);
  persistOrEscalate(NVS_KEYS::REBOOT_COUNT, n, sizeof(uint8_t), /*securityRelevant=*/false);
}

void saveRebootReason(uint8_t reason) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putUChar(NVS_KEYS::REBOOT_REASON, reason);
  persistOrEscalate(NVS_KEYS::REBOOT_REASON, n, sizeof(uint8_t), /*securityRelevant=*/false);
}

uint8_t loadRebootReason() {
  if (!ensureInitialized())
    return 0xFF;
  return _state.prefs.getUChar(NVS_KEYS::REBOOT_REASON, 0xFF);
}

bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!ensureInitialized())
    return false;
  size_t privRead = _state.prefs.getBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubRead = _state.prefs.getBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  if (privRead != 32 || pubRead != 32)
    return false;
  uint32_t stored = _state.prefs.getUInt(NVS_KEYS::KEYPAIR_CRC, 0xFFFFFFFF);
  if (stored == 0xFFFFFFFF)
    return false;
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = crc16(both, 64);
  if (static_cast<uint16_t>(stored) != computed) {
    LATTICE_LOGLN("NVS", "Keypair CRC mismatch", lattice::utils::LogLevel::LOG_WARN);
    return false;
  }
  return true;
}

void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  // securityRelevant=true on all three: this is the device's long-term
  // identity keypair — the same tier as MESH_KEY/KNOWN_MASTER_MAC below.
  size_t nPriv = _state.prefs.putBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  persistOrEscalate(NVS_KEYS::PRIVATE_KEY, nPriv, 32, /*securityRelevant=*/true);
  size_t nPub = _state.prefs.putBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  persistOrEscalate(NVS_KEYS::PUBLIC_KEY, nPub, 32, /*securityRelevant=*/true);
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = crc16(both, 64);
  size_t nCrc = _state.prefs.putUInt(NVS_KEYS::KEYPAIR_CRC, static_cast<uint32_t>(crc));
  persistOrEscalate(NVS_KEYS::KEYPAIR_CRC, nCrc, sizeof(uint32_t), /*securityRelevant=*/true);
  logOperation("Keypair saved");
}

bool loadEnrolledFlag() {
  if (!ensureInitialized())
    return false;
  return _state.prefs.getBool(NVS_KEYS::ENROLLED_FLAG, false);
}

void saveEnrolledFlag(bool enrolled) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putBool(NVS_KEYS::ENROLLED_FLAG, enrolled);
  persistOrEscalate(NVS_KEYS::ENROLLED_FLAG, n, 1, /*securityRelevant=*/false);
}

uint32_t loadBootEpoch() {
  if (!ensureInitialized())
    return 0;
  if (_state.isDevMode)
    return _state.devEpoch;
  return _state.prefs.getUInt(NVS_KEYS::BOOT_EPOCH, 0);
}

void saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized())
    return;
  if (_state.isDevMode) {
    _state.devEpoch = epoch;
    return;
  }
  size_t n = _state.prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch);
  persistOrEscalate(NVS_KEYS::BOOT_EPOCH, n, sizeof(uint32_t), /*securityRelevant=*/true);
}

bool loadKnownMasterMac(uint8_t* mac) {
  if (!ensureInitialized())
    return false;
  size_t read = _state.prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
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

void saveKnownMasterMac(const uint8_t* mac) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  persistOrEscalate(NVS_KEYS::KNOWN_MASTER_MAC, n, 6, /*securityRelevant=*/true);
  logOperation("Known master MAC saved");
}

void clearKnownMasterMac() {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  _state.prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC);
  logOperation("Known master MAC cleared");
}

bool loadKnownMasterMacSecondary(uint8_t* mac) {
  if (!ensureInitialized())
    return false;
  size_t read = _state.prefs.getBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
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

void saveKnownMasterMacSecondary(const uint8_t* mac) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC_SEC, mac, 6);
  persistOrEscalate(NVS_KEYS::KNOWN_MASTER_MAC_SEC, n, 6, /*securityRelevant=*/true);
  logOperation("Known secondary master MAC saved");
}

void clearKnownMasterMacSecondary() {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  _state.prefs.remove(NVS_KEYS::KNOWN_MASTER_MAC_SEC);
}

lattice::config::TxPowerPreset loadTxPowerPreset() {
  if (!ensureInitialized())
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  uint8_t val = _state.prefs.getUChar(NVS_KEYS::TX_POWER_PRESET, 0xFF);
  if (val > 2)
    return lattice::config::DEFAULT_TX_POWER_PRESET;
  return static_cast<lattice::config::TxPowerPreset>(val);
}

void saveTxPowerPreset(lattice::config::TxPowerPreset preset) {
  if (!ensureInitialized() || _state.isDevMode)
    return;
  size_t n = _state.prefs.putUChar(NVS_KEYS::TX_POWER_PRESET, static_cast<uint8_t>(preset));
  persistOrEscalate(NVS_KEYS::TX_POWER_PRESET, n, sizeof(uint8_t), /*securityRelevant=*/false);
  logOperation("TX power preset saved");
}

uint8_t loadNodeId() {
  if (!ensureInitialized())
    return 0;
  return _state.prefs.getUChar(NVS_KEYS::NODE_ID, 0);
}

void saveNodeId(uint8_t nodeId) {
  if (!ensureInitialized())
    return;
  size_t n = _state.prefs.putUChar(NVS_KEYS::NODE_ID, nodeId);
  persistOrEscalate(NVS_KEYS::NODE_ID, n, sizeof(uint8_t), /*securityRelevant=*/false);
  logOperation("saveNodeId");
}

void clearAll() {
  if (!ensureInitialized())
    return;
  _state.prefs.clear();
  logOperation("All NVS cleared");
}

void dumpEEPROM() {
  LATTICE_LOGLN("NVS", "NVS dump not implemented (use idf.py nvs-dump)",
                lattice::utils::LogLevel::LOG_INFO);
}

#ifdef UNIT_TEST
bool isInitializedForTest() {
  return _state.isInitialized;
}

detail::State& debugStateForTest() {
  return _state;
}
#endif

} // namespace eeprom
} // namespace lattice
