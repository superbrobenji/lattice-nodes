#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include "E2ECrypto.h"
#include "../../project_config.h"
#include "src/network/MacEq.h"
#include "src/network/mac_table.h"
#include "src/network/mem.h"

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
//
// Phase G audit item B (role split): the backing storage is heap-allocated
// (mirrors RouteTable's role-conditional allocation from Phase B) and defaults
// to LATTICE_E2E_KEYCACHE_MAX (the master size) so standalone construction —
// including existing unit tests that new up an E2EKeyStore directly — behaves
// exactly as before. Mesh::reevaluateRouteTable() calls setCapacity() once the
// real node role is known, shrinking leaves down to LATTICE_E2E_KEYCACHE_MAX_LEAF.
class E2EKeyStore {
public:
  E2EKeyStore() { setCapacity(config::LATTICE_E2E_KEYCACHE_MAX); }
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

  // Resize the cache to `maxEntries` slots. Idempotent no-op if the capacity
  // already matches (avoids reallocating — and losing every cached key — on
  // every reevaluateRouteTable() call when the role hasn't actually changed).
  // Reallocating drops any previously cached keys (cheap to re-derive); callers
  // set this once at boot / on role change, before steady-state traffic.
  void setCapacity(size_t maxEntries) {
    if (maxEntries == capacity_)
      return;
    entries = maxEntries > 0 ? std::make_unique<Entry[]>(maxEntries) : nullptr;
    capacity_ = maxEntries;
    nextSlot = 0;
  }

  bool getKeys(const uint8_t* mac, const uint8_t* ownPriv32, const uint8_t* peerPub32,
               const uint8_t** kUpOut, const uint8_t** kDownOut) {
    if (capacity_ == 0)
      return false;
    // Thinned via lattice::mac_table::find (Phase H2 audit item Y).
    size_t idx = lattice::mac_table::find(entries.get(), capacity_, sizeof(Entry),
                                          offsetof(Entry, mac), mac);
    if (idx != SIZE_MAX && entries[idx].valid) {
      *kUpOut = entries[idx].kUp;
      *kDownOut = entries[idx].kDown;
      return true;
    }
    if (!peerPub32)
      return false;
    // Thinned via lattice::mem::is_zero (Phase H2 audit item Z).
    if (lattice::mem::is_zero(peerPub32, 32))
      return false;
    Entry& e = entries[nextSlot];
    nextSlot = (nextSlot + 1) % capacity_;
    crypto::deriveE2EKeys(ownPriv32, peerPub32, e.kUp, e.kDown);
    memcpy(e.mac, mac, 6);
    e.valid = true;
    *kUpOut = e.kUp;
    *kDownOut = e.kDown;
    return true;
  }

  void clear() {
    if (capacity_ > 0)
      memset(entries.get(), 0, capacity_ * sizeof(Entry));
    nextSlot = 0;
  }

private:
  struct Entry {
    uint8_t mac[6];
    bool valid;
    uint8_t kUp[32];
    uint8_t kDown[32];
  };
  std::unique_ptr<Entry[]> entries;
  size_t capacity_{0};
  size_t nextSlot{0};
};

} // namespace mesh
} // namespace lattice
