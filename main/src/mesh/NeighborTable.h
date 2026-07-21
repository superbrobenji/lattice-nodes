#pragma once
#include <cstdint>
#include <cstring>
#include "../../project_config.h"

namespace lattice {
namespace mesh {

// RAM-only table of forwarding candidates toward the master (spec §3), learned
// from overheard master beacons. Holds ROUTING ONLY — never key material and
// never consulted for E2E crypto (spec §2 trust split). Separate from
// PeerRegistry, whose enrollment-only add rule is unchanged.
//
// A neighbor's masterDistance is beacon.hop_count of the best beacon heard from
// it — hop_count is the SENDER's (last_hop's) own distance to the master, one
// less than the receiving node's resulting distance (which is hop_count + 1);
// the neighbor's mac is that beacon's last_hop_mac_address. Next hop = freshest
// neighbor strictly closer to the master than we are.
class NeighborTable {
public:
  NeighborTable() = default;
  NeighborTable(const NeighborTable&) = delete;
  NeighborTable& operator=(const NeighborTable&) = delete;
  // Move is fine (and needed so composing types — e.g. Mesh, which now holds
  // one of these — stay returnable-by-value/relocatable, notably in test
  // factory helpers). The table holds no pointers or owned resources, only
  // fixed-size POD entries.
  NeighborTable(NeighborTable&&) = default;
  NeighborTable& operator=(NeighborTable&&) = default;

  // Insert or update the neighbor. On a full table with no existing slot for
  // this mac, evict a stale entry first, else the entry farthest from the master.
  void observe(const uint8_t* mac, uint8_t masterDistance, uint32_t nowMillis) {
    Entry* slot = findSlot(mac);
    if (!slot)
      slot = allocateSlot(nowMillis);
    memcpy(slot->mac, mac, 6);
    slot->masterDistance = masterDistance;
    slot->lastSeenMillis = nowMillis;
    slot->valid = true;
  }

  // Freshest in-range neighbor with masterDistance strictly less than ownDistance.
  bool selectNextHop(uint8_t ownDistance, uint32_t nowMillis, uint8_t* outMac) const {
    const Entry* best = nullptr;
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i) {
      const Entry& e = entries[i];
      if (!e.valid)
        continue;
      if (nowMillis - e.lastSeenMillis >= config::STALE_PEER_THRESHOLD_MS)
        continue;
      if (e.masterDistance >= ownDistance)
        continue;
      // Freshest = largest lastSeenMillis (most recent). nowMillis is monotonic
      // per boot; observe() only ever stores nowMillis values, so no wrap concern
      // within the staleness window.
      if (!best || e.lastSeenMillis > best->lastSeenMillis)
        best = &e;
    }
    if (!best)
      return false;
    memcpy(outMac, best->mac, 6);
    return true;
  }

  bool contains(const uint8_t* mac) const {
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (entries[i].valid && memcmp(entries[i].mac, mac, 6) == 0)
        return true;
    return false;
  }

  void clear() { memset(entries, 0, sizeof(entries)); }

private:
  struct Entry {
    uint8_t mac[6];
    uint8_t masterDistance;
    bool valid;
    uint32_t lastSeenMillis;
  };
  Entry entries[config::LATTICE_NEIGHBOR_MAX]{};

  Entry* findSlot(const uint8_t* mac) {
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (entries[i].valid && memcmp(entries[i].mac, mac, 6) == 0)
        return &entries[i];
    return nullptr;
  }

  // Pick a slot for a new neighbor: first invalid, else a stale one, else the
  // entry with the largest masterDistance (farthest from master).
  Entry* allocateSlot(uint32_t nowMillis) {
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (!entries[i].valid)
        return &entries[i];
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (nowMillis - entries[i].lastSeenMillis >= config::STALE_PEER_THRESHOLD_MS)
        return &entries[i];
    Entry* farthest = &entries[0];
    for (size_t i = 1; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (entries[i].masterDistance > farthest->masterDistance)
        farthest = &entries[i];
    return farthest;
  }
};

} // namespace mesh
} // namespace lattice
