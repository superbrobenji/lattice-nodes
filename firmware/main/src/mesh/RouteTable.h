#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "../../project_config.h"
#include "src/network/MacEq.h"
#include "src/network/mac_table.h"

namespace lattice {
namespace mesh {

// Master-side RAM route table (spec §4): node MAC -> the relay path from the
// origin to the master, in origin-to-master forward order, as learned from that
// node's most recent route report. RAM-only, rebuilt from reports. Routing only,
// no key material. Bounded by LATTICE_ROUTE_TABLE_MAX; evicts the oldest entry.
class RouteTable {
public:
  RouteTable() = default;
  RouteTable(const RouteTable&) = delete;
  RouteTable& operator=(const RouteTable&) = delete;
  // Move is fine (and needed so composing types — e.g. Mesh, which now holds
  // one of these — stay returnable-by-value/relocatable, notably in test
  // factory helpers). The table holds no pointers or owned resources, only
  // fixed-size POD entries. Mirrors NeighborTable's rationale.
  RouteTable(RouteTable&&) = default;
  RouteTable& operator=(RouteTable&&) = default;

  void record(const uint8_t* nodeMac, const uint8_t* path, uint8_t pathLen, uint32_t nowMillis) {
    if (pathLen > config::MAX_HOPS)
      return; // parse-safety: never store an overlong path
    Entry* slot = findSlot(nodeMac);
    if (!slot)
      slot = allocateSlot();
    memcpy(slot->nodeMac, nodeMac, 6);
    slot->pathLen = pathLen;
    if (pathLen)
      memcpy(slot->path, path, static_cast<size_t>(pathLen) * 6);
    slot->lastSeenMillis = nowMillis;
    slot->valid = true;
  }

  // Thinned via lattice::mac_table::find (Phase H2 audit item Y).
  bool lookup(const uint8_t* nodeMac, uint8_t* pathOut, uint8_t* pathLenOut) const {
    size_t idx = lattice::mac_table::find(entries, config::LATTICE_ROUTE_TABLE_MAX, sizeof(Entry),
                                          offsetof(Entry, nodeMac), nodeMac);
    if (idx == SIZE_MAX || !entries[idx].valid)
      return false;
    const Entry& e = entries[idx];
    *pathLenOut = e.pathLen;
    if (e.pathLen)
      memcpy(pathOut, e.path, static_cast<size_t>(e.pathLen) * 6);
    return true;
  }

  void clear() { memset(entries, 0, sizeof(entries)); }

private:
  struct Entry {
    uint8_t nodeMac[6];
    uint8_t pathLen;
    bool valid;
    uint32_t lastSeenMillis;
    uint8_t path[config::MAX_HOPS * 6]; // 48 bytes (MAX_HOPS=8), matches route_path[]
  };
  Entry entries[config::LATTICE_ROUTE_TABLE_MAX]{};

  // Thinned via lattice::mac_table::find (Phase H2 audit item Y).
  Entry* findSlot(const uint8_t* nodeMac) {
    size_t idx = lattice::mac_table::find(entries, config::LATTICE_ROUTE_TABLE_MAX, sizeof(Entry),
                                          offsetof(Entry, nodeMac), nodeMac);
    if (idx == SIZE_MAX || !entries[idx].valid)
      return nullptr;
    return &entries[idx];
  }

  // Thinned via lattice::mac_table::evict_oldest_by_ts (Phase H2 audit item Y).
  Entry* allocateSlot() {
    for (size_t i = 0; i < config::LATTICE_ROUTE_TABLE_MAX; ++i)
      if (!entries[i].valid)
        return &entries[i];
    size_t idx = lattice::mac_table::evict_oldest_by_ts(
        entries, config::LATTICE_ROUTE_TABLE_MAX, sizeof(Entry), offsetof(Entry, lastSeenMillis));
    return &entries[idx];
  }
};

} // namespace mesh
} // namespace lattice
