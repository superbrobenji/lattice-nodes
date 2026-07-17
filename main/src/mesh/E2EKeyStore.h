#pragma once
#include <cstdint>
#include <cstring>
#include "E2ECrypto.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

// RAM-only cache of derived E2E key pairs, one entry per peer MAC (spec §2).
// Derivation costs an X25519 exchange (~ms on ESP32) — cache so the periodic
// uplink path never re-derives. Round-robin overwrite when full; wrong evictions
// only cost a re-derivation.
//
// IMPORTANT: Pointers returned via kUpOut/kDownOut are invalidated by any
// subsequent getKeys() call that causes an eviction. Callers must use them
// immediately and must NOT cache them across getKeys() calls.
class E2EKeyStore {
public:
  E2EKeyStore() = default;
  E2EKeyStore(const E2EKeyStore&) = delete;
  E2EKeyStore& operator=(const E2EKeyStore&) = delete;
  // Move is fine (and needed so composing types — e.g. Mesh, which now holds
  // one of these — stay returnable-by-value/relocatable, notably in test
  // factory helpers). The cache holds no pointers or owned resources, only
  // fixed-size POD entries; any raw pointer a caller obtained from getKeys()
  // is already documented above as invalidated by any subsequent structural
  // change, so a move is no riskier than an eviction.
  E2EKeyStore(E2EKeyStore&&) = default;
  E2EKeyStore& operator=(E2EKeyStore&&) = default;
  bool getKeys(const uint8_t mac[6], const uint8_t* ownPriv32, const uint8_t* peerPub32,
               const uint8_t** kUpOut, const uint8_t** kDownOut) {
    for (size_t i = 0; i < config::LATTICE_E2E_KEYCACHE_MAX; ++i) {
      if (entries[i].valid && memcmp(entries[i].mac, mac, 6) == 0) {
        *kUpOut = entries[i].kUp;
        *kDownOut = entries[i].kDown;
        return true;
      }
    }
    if (!peerPub32)
      return false;
    bool allZero = true;
    for (int i = 0; i < 32; ++i) {
      if (peerPub32[i] != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero)
      return false;
    Entry& e = entries[nextSlot];
    nextSlot = (nextSlot + 1) % config::LATTICE_E2E_KEYCACHE_MAX;
    crypto::deriveE2EKeys(ownPriv32, peerPub32, e.kUp, e.kDown);
    memcpy(e.mac, mac, 6);
    e.valid = true;
    *kUpOut = e.kUp;
    *kDownOut = e.kDown;
    return true;
  }

  void clear() {
    memset(entries, 0, sizeof(entries));
    nextSlot = 0;
  }

private:
  struct Entry {
    uint8_t mac[6];
    bool valid;
    uint8_t kUp[32];
    uint8_t kDown[32];
  };
  Entry entries[config::LATTICE_E2E_KEYCACHE_MAX]{};
  size_t nextSlot{0};
};

} // namespace mesh
} // namespace lattice
