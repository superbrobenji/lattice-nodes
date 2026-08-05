#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "MacEq.h"

// Shared "MAC-keyed table" skeleton (Phase H2 audit item Y): NeighborTable,
// RouteTable, E2EKeyStore, ReplayCache, and PeerRegistry each reimplement the
// same "linear-scan-by-MAC" and "evict-the-oldest-by-timestamp" primitives
// over their own POD entry arrays (~180 duplicated lines). Free functions
// here dedup that skeleton WITHOUT templates (avoids a template instantiated
// per entry type bloating flash) — each entry type is described to these
// helpers purely by byte stride and field offset, so one instantiation of
// each helper serves every caller.
//
// Complementary to Phase G audit item Q (lattice::mac::eq, network/MacEq.h),
// which fixed the MAC-compare idiom itself; this fixes the loop around it.
//
// Callers remain responsible for any "valid"/"used" bit check on the entry
// at a returned index — these helpers only locate by MAC or by timestamp,
// they know nothing about a validity flag's offset or meaning.

namespace lattice {
namespace mac_table {

// Linear scan for the entry whose 6-byte MAC field (at byte offset
// mac_offset within each stride-byte entry) equals `mac`. Returns the
// matching index, or SIZE_MAX if none of the n entries match.
//
// Does NOT consult any "valid"/"used" flag. Every current caller reserves
// unused slots by zeroing their MAC field (aggregate-init `{}` plus
// memset-based clear()/init(), and no caller flips a slot invalid without
// also zeroing it) — since a real device MAC is never all-zero, an unused
// slot can never spuriously match a real lookup key, so scanning the full
// entry array (not just the "valid" prefix) reproduces the original
// valid-gated scans' results exactly. Callers still re-check the flag at
// the returned index defensively (see NeighborTable::findSlot etc.).
inline size_t find(const void* entries, size_t n, size_t stride, size_t mac_offset,
                    const uint8_t mac[6]) {
  const uint8_t* base = static_cast<const uint8_t*>(entries);
  for (size_t i = 0; i < n; ++i) {
    if (lattice::mac::eq(base + i * stride + mac_offset, mac))
      return i;
  }
  return SIZE_MAX;
}

// Index of the entry with the smallest uint32_t timestamp (at byte offset
// ts_offset within each stride-byte entry) across all n entries. On a tie,
// the lowest index wins — matches every current caller's hand-rolled
// "start at index 0, replace only on strictly-less" loop.
//
// Undefined (returns 0 without reading past entries[0]) for n == 0; every
// current caller already guards this with a preceding "any free slot?"
// scan before falling back to eviction.
inline size_t evict_oldest_by_ts(const void* entries, size_t n, size_t stride, size_t ts_offset) {
  const uint8_t* base = static_cast<const uint8_t*>(entries);
  size_t oldestIdx = 0;
  if (n == 0)
    return oldestIdx;
  uint32_t oldestTs;
  memcpy(&oldestTs, base + ts_offset, sizeof(uint32_t));
  for (size_t i = 1; i < n; ++i) {
    uint32_t ts;
    memcpy(&ts, base + i * stride + ts_offset, sizeof(uint32_t));
    if (ts < oldestTs) {
      oldestTs = ts;
      oldestIdx = i;
    }
  }
  return oldestIdx;
}

} // namespace mac_table
} // namespace lattice
