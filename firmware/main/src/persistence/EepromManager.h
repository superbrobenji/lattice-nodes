#ifndef EEPROM_MANAGER_H
#define EEPROM_MANAGER_H

#include <Arduino.h>
// Phase I Task 4: nvs_flash direct — replaces the Preferences wrapper with
// straight ESP-IDF nvs_flash calls (nvs_open/nvs_get_*/nvs_set_*/nvs_commit).
#include <nvs.h>
#include <nvs_flash.h>
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
// Phase I Task 6 (JJ): retired — the peer list moved from this single combined
// blob to per-record "peer0".."peer9" keys (built at runtime by
// EepromManager.cpp's peerKey() helper) so PeerRegistry can load/save one
// record at a time instead of a 380-byte stack buffer. Kept (unused) as a
// migration/rollback reference, not read or written anywhere anymore.
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

} // namespace utils
} // namespace lattice

// Phase H2 item AA: Meyers singleton -> namespace of free functions backed by
// file-static state (EepromManager.cpp). Each unique EepromManager::getInstance()
// callsite used to cost a __cxa_guard_acquire/release prologue (~40B + a byte
// flag); free functions drop that entirely. Public API (function names +
// signatures) is unchanged from the old class's methods.
namespace lattice {
namespace eeprom {

namespace detail {
// Groups the module's mutable state into one struct so the e2e test harness
// (tests/e2e/harness/NodeContext.cpp) can still snapshot/restore it as a flat
// byte image per simulated node -- the same technique it used against the
// old singleton object.
//
// Phase I Task 4: no persistent Preferences/nvs_handle_t member anymore --
// each operation opens/commits/closes its own short-lived nvs_handle_t (see
// EepromManager.cpp), so this struct is a plain POD (safe for the harness's
// memcpy-based image copy).
struct State {
  bool isInitialized = false;
  bool isDevMode = false;
  uint32_t devEpoch = 0; // DEV_MODE RAM-only monotonic boot-epoch seed (issue #43)
};
} // namespace detail

bool init();
void setDevMode(bool devMode);
bool getDevMode();

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

// Phase I Task 6 (JJ): per-key peer-record accessors — each record lives at
// its own "peer0".."peer9" NVS key instead of one combined "peers" blob
// (real nvs_get_blob has no offset/partial-read mode, so per-key naming is
// the only way for a caller to stream records one at a time without holding
// the whole MAX_PEERS*PEER_RECORD_SIZE array on the stack). loadPeerList/
// savePeerList above are now thin loops over these three, kept for their
// existing callers (main.cpp's default-peer bootstrap, host tests) — the
// per-record functions exist so PeerRegistry can avoid the combined buffer
// entirely. `record` must point to EEPROM_SIZES::PEER_RECORD_SIZE bytes.
// index must be < EEPROM_SIZES::MAX_PEERS.
bool loadPeerRecord(uint8_t index, uint8_t* record);
void savePeerRecord(uint8_t index, const uint8_t* record);
void erasePeerRecord(uint8_t index);

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

inline void flushIfDirty() {}
inline void forceFlush() {}

void clearAll();
void dumpEEPROM();

#ifdef UNIT_TEST
bool isInitializedForTest();
// Raw access to the module's state blob for the e2e harness's byte-image
// snapshot/restore (tests/e2e/harness/NodeContext.cpp).
detail::State& debugStateForTest();
#endif

} // namespace eeprom
} // namespace lattice

#endif // EEPROM_MANAGER_H
