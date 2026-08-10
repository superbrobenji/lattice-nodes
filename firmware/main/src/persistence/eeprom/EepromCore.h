#ifndef LATTICE_EEPROM_CORE_H
#define LATTICE_EEPROM_CORE_H

#include <cstdint>
#include <cstddef>
#include <nvs.h>
#include <nvs_flash.h>
#include "src/logging/Logger.h"
#include "../../../project_config.h"

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
// EepromPeers.cpp's peerKey() helper) so PeerRegistry can load/save one
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

// Phase C: EepromManager split into persistence/eeprom/ domain files (audit
// finding 4). This header carries two things with different audiences: the
// public lifecycle API (init/setDevMode/getDevMode/flushIfDirty/forceFlush/
// clearAll) plus the EEPROM_SIZES/NVS_KEYS constants above, which any
// consumer may legitimately include (main.cpp, ButtonHandler.h, Mesh.cpp,
// and PeerRegistry.h all do today) — and two internal-only pieces below,
// detail:: (module state, reachable only via the UNIT_TEST-gated
// debugStateForTest() the e2e harness uses to snapshot/restore it) and
// core_internal:: (the KV-primitive layer). Neither is meant for use
// outside persistence/eeprom/: core_internal:: exists so the other
// eeprom/*.cpp domain files can call into it without each re-declaring its
// own copy, and external consumers should include only the domain
// header(s) they actually call into, which is the fix for "any consumer
// can reach any persistence function".
namespace lattice {
namespace eeprom {

namespace detail {
// Groups the module's mutable state into one struct so the e2e test harness
// (tests/e2e/harness/NodeContext.cpp) can still snapshot/restore it as a flat
// byte image per simulated node -- the same technique it used against the
// old singleton object.
struct State {
  bool isInitialized = false;
  bool isDevMode = false;
  uint32_t devEpoch = 0; // DEV_MODE RAM-only monotonic boot-epoch seed (issue #43)
};
} // namespace detail

bool init();
void setDevMode(bool devMode);
bool getDevMode();
inline void flushIfDirty() {}
inline void forceFlush() {}
void clearAll();
void dumpEEPROM();

#ifdef UNIT_TEST
bool isInitializedForTest();
detail::State& debugStateForTest();
#endif

namespace core_internal {
// Shared by every eeprom/*.cpp domain file. Not part of the public
// lattice::eeprom API — declared here so those files can call into it
// without each re-declaring its own copy.
bool ensureInitialized();
void logOperation(const char* operation, const char* details = nullptr);
uint16_t crc16(const uint8_t* data, size_t len);
bool persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant);
uint8_t nvsGetU8(const char* key, uint8_t defaultValue);
size_t nvsPutU8(const char* key, uint8_t value);
uint32_t nvsGetU32(const char* key, uint32_t defaultValue);
size_t nvsPutU32(const char* key, uint32_t value);
bool nvsGetBool(const char* key, bool defaultValue);
size_t nvsPutBool(const char* key, bool value);
size_t nvsGetBytes(const char* key, void* buf, size_t maxLen);
size_t nvsPutBytes(const char* key, const void* buf, size_t len);
bool nvsRemove(const char* key);
bool nvsHasKey(const char* key);
void peerKey(uint8_t index, char* out, size_t outSize);
bool isDevModeInternal(); // domain files need this without calling public getDevMode() semantics
// Phase C: DEV_MODE RAM-only boot-epoch seed lives in detail::State (private
// to EepromCore.cpp) — EepromDiagnostics.cpp needs read/write access to it
// without reaching into _state directly, mirroring isDevModeInternal() above.
uint32_t& devEpochRef();
} // namespace core_internal

} // namespace eeprom
} // namespace lattice

#endif // LATTICE_EEPROM_CORE_H
