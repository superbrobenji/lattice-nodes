#pragma once
#include <cstdint>
#include <cstring>
#include "../../project_config.h"
#include "src/network/MacEq.h"

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

  // Insert-and-fold variant of observe() + minFreshDistance() (post-Phase-G
  // audit item X): Mesh::processMasterBeacon used to call the two back to
  // back, walking the whole table twice per beacon RX. This does both in one
  // pass over `entries` — it locates (or picks) the slot for `mac` AND
  // accumulates the min masterDistance across the OTHER fresh entries in the
  // same loop, then folds the just-written entry's distance in afterwards (it
  // is always "fresh" relative to nowMillis — age 0 — so it always
  // participates in the min, mirroring minFreshDistance()'s semantics).
  // Returns the new min fresh masterDistance across the whole table (0xFF if
  // none are fresh), exactly what
  // `observe(mac, masterDistance, nowMillis); return
  // minFreshDistance(nowMillis);` would have returned. observe()/
  // minFreshDistance() are kept as-is for other callers (e.g. unit tests
  // exercising them independently).
  uint8_t observeAndMinDistance(const uint8_t* mac, uint8_t masterDistance, uint32_t nowMillis) {
    size_t matchIdx = config::LATTICE_NEIGHBOR_MAX;
    size_t firstInvalidIdx = config::LATTICE_NEIGHBOR_MAX;
    size_t firstStaleIdx = config::LATTICE_NEIGHBOR_MAX;
    size_t farthestIdx = config::LATTICE_NEIGHBOR_MAX;
    uint8_t farthestDistance = 0;
    uint8_t bestOther = 0xFF;

    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i) {
      Entry& e = entries[i];
      if (!e.valid) {
        if (firstInvalidIdx == config::LATTICE_NEIGHBOR_MAX)
          firstInvalidIdx = i;
        continue;
      }
      if (lattice::mac::eq(e.mac, mac)) {
        // Existing slot for this neighbor — will be overwritten below, so it
        // is excluded from bestOther/allocation bookkeeping (its stale
        // pre-update distance must not leak into the new min).
        matchIdx = i;
        continue;
      }
      bool stale = (nowMillis - e.lastSeenMillis) >= config::STALE_PEER_THRESHOLD_MS;
      if (stale) {
        if (firstStaleIdx == config::LATTICE_NEIGHBOR_MAX)
          firstStaleIdx = i;
      } else if (e.masterDistance < bestOther) {
        bestOther = e.masterDistance;
      }
      // Farthest-by-distance fallback (mirrors allocateSlot()'s third pass,
      // including its tie-break: strictly-greater only, so on a tie the
      // lowest-index entry wins — same as that loop's `> farthest->distance`
      // starting from entries[0]).
      if (farthestIdx == config::LATTICE_NEIGHBOR_MAX || e.masterDistance > farthestDistance) {
        farthestDistance = e.masterDistance;
        farthestIdx = i;
      }
    }

    size_t slotIdx;
    if (matchIdx != config::LATTICE_NEIGHBOR_MAX)
      slotIdx = matchIdx;
    else if (firstInvalidIdx != config::LATTICE_NEIGHBOR_MAX)
      slotIdx = firstInvalidIdx;
    else if (firstStaleIdx != config::LATTICE_NEIGHBOR_MAX)
      slotIdx = firstStaleIdx;
    else
      slotIdx = farthestIdx;

    Entry& slot = entries[slotIdx];
    memcpy(slot.mac, mac, 6);
    slot.masterDistance = masterDistance;
    slot.lastSeenMillis = nowMillis;
    slot.valid = true;

    return masterDistance < bestOther ? masterDistance : bestOther;
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

  // Smallest masterDistance across valid entries within STALE_PEER_THRESHOLD_MS
  // of nowMillis. 0xFF if none are fresh. Used by Mesh::processMasterBeacon to
  // derive currentMaster.distance from live neighbor state (issue #45).
  uint8_t minFreshDistance(uint32_t nowMillis) const {
    uint8_t best = 0xFF;
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i) {
      const Entry& e = entries[i];
      if (!e.valid)
        continue;
      if (nowMillis - e.lastSeenMillis >= config::STALE_PEER_THRESHOLD_MS)
        continue;
      if (e.masterDistance < best)
        best = e.masterDistance;
    }
    return best;
  }

  bool contains(const uint8_t* mac) const {
    for (size_t i = 0; i < config::LATTICE_NEIGHBOR_MAX; ++i)
      if (entries[i].valid && lattice::mac::eq(entries[i].mac, mac))
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
      if (entries[i].valid && lattice::mac::eq(entries[i].mac, mac))
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
