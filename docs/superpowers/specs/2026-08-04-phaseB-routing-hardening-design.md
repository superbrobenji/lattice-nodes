# Phase B — Routing hardening (#45 + #46 + #51)

**Status:** Approved
**Date:** 2026-08-04
**Repo:** lattice-nodes
**Scope:** ESP32 firmware only. No wire-format changes. No cross-repo work.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase B)

## Context

Three unrelated-but-adjacent routing defects filed after the Phase 1-5 whole-branch review. All three are Low severity individually; bundled because they cluster in `Mesh` / `NeighborTable` / `ReplayCache` / `RouteTable`, share test fixtures, and constitute the umbrella-spec's Phase B block. Umbrella spec authorises the three approaches chosen below.

## Design

Three components. Each isolated. Each independently testable. Each ends with its own commit.

### 1. #45 — Distance derived from `min(fresh-neighbor) + 1`

**Current behaviour** (`Mesh::processMasterBeacon`, `firmware/main/src/mesh/Mesh.cpp:756–763`):

```cpp
uint8_t newDistance = msg.hop_count + 1;
if (currentMaster.distance == 0xFF ||
    MacAddress(currentMaster.mac) != MacAddress(msg.origin_mac_address) ||
    newDistance < currentMaster.distance) {
  memcpy(currentMaster.mac, msg.origin_mac_address, 6);
  currentMaster.distance = newDistance;
}
```

Sticky-min: `currentMaster.distance` never rises for the same master. If the shortest path dies and only a longer path survives, `NeighborTable::selectNextHop` — which requires a next hop *strictly closer* than `currentMaster.distance` — rejects the surviving path, and uplink drops until `checkMasterTimeout` fires (~9 s).

**New behaviour.** After the existing `neighbors.observe(msg.last_hop_mac_address, msg.hop_count, millis())` call, derive `currentMaster.distance` from the freshest neighbor state:

```cpp
memcpy(currentMaster.mac, msg.origin_mac_address, 6);
uint8_t min_d = neighbors.minFreshDistance(millis());  // 0xFF if none fresh
uint8_t derived = (min_d == 0xFF) ? 0xFF : static_cast<uint8_t>(min_d + 1);
if (derived != currentMaster.distance) {
  currentMaster.distance = derived;
  Logger::logln("MESH", "Route distance derived: " + String(derived), LogLevel::LOG_INFO);
}
```

`NeighborTable` gains one new const method:

```cpp
// Return the smallest masterDistance across valid + within-STALE_PEER_THRESHOLD_MS
// entries. Return 0xFF if none are fresh.
uint8_t minFreshDistance(uint32_t nowMillis) const;
```

**Oscillation review.** The derivation is a pure function of `NeighborTable` state at the moment of a beacon receipt. Distance can only flap if neighbor state itself flaps. `neighbors.observe` only overwrites its own slot per-neighbor; entries only age out on `STALE_PEER_THRESHOLD_MS`. Two paths of different length both delivering beacons produce two entries with different `masterDistance`; `minFreshDistance` returns the smaller. When the shorter-path neighbor ages out, `minFreshDistance` rises to the next-shortest — one-time, monotone rise. No flap.

### 2. #46 — Per-origin high-water `ReplayCache`

**Current behaviour** (`firmware/main/src/mesh/ReplayCache.h`): 16-entry round-robin ring keyed on `(mac, epoch, seq)` exact tuple. When more than 16 distinct frames arrive, oldest entries evict; a subsequent replay of an evicted frame is accepted as new.

**New behaviour.** Per-origin high-water — one entry per origin MAC, storing the highest-seen `(epoch, seq)`. Accept iff strictly newer than the stored tuple.

```cpp
struct Entry {
  uint8_t mac[6];
  uint32_t epoch;
  uint16_t seq;
  uint32_t lastSeenMs;  // for LRU eviction on full table
  bool used;
};

Entry cache[config::LATTICE_REPLAY_MAX_ORIGINS];  // compile-time knob, default 16

bool isReplay(const mesh_message& msg, uint32_t nowMs);
```

`isReplay(msg, nowMs)` semantics:

1. Find slot with `used && memcmp(mac, msg.origin_mac_address, 6) == 0`.
2. Slot found:
   - `newer = (msg.epoch_num > slot.epoch) || (msg.epoch_num == slot.epoch && msg.seq_num > slot.seq)`.
   - `!newer` → return `true` (drop).
   - `newer` → update slot's `(epoch, seq, lastSeenMs)`, return `false` (accept).
3. Slot not found:
   - Allocate: first `!used` slot; else evict entry with smallest `lastSeenMs`.
   - Initialise slot with the incoming `(mac, epoch, seq, lastSeenMs = nowMs, used = true)`, return `false` (accept).

**Signature change:** `isReplay` now takes `nowMs`. Callers (`Mesh.cpp` msg-handling paths) pass `millis()`.

**Knob.** Add to `firmware/main/project_config.h`:

```cpp
namespace config {
// Per-origin replay-cache size. Bounds memory to LATTICE_REPLAY_MAX_ORIGINS ×
// sizeof(Entry). Should be sized to expected concurrent origins × 1.5.
constexpr size_t LATTICE_REPLAY_MAX_ORIGINS = 16;
}
```

**Behaviour change note.** In the old ring, > 16 distinct origins in a round rotate slots but each frame is still checked against every remaining slot; a replay is accepted only after ≥ 16 intervening distinct frames evict its slot. In the new per-origin scheme, > 16 distinct origins force LRU eviction: an ejected origin's next frame is accepted as first-ever, so an attacker could replay a genuine older frame from that origin if they can first flood > `LATTICE_REPLAY_MAX_ORIGINS` distinct origins. Two counters:
1. AEAD still authenticates content; attacker cannot forge, only re-deliver.
2. Deployer sizes the knob to `expected max concurrent origins × 1.5` — 16 matches the current default cap.

Same degrade-not-fail bound as the old ring, but with tighter per-origin guarantee below the eviction threshold.

**Fields preserved unchanged:** `bootEpoch`, `txSeqNum`, `nextSeq()`, `lastRelayedEpoch`, `lastRelayedSeqNum`, `init(epoch)`. Public surface used elsewhere (`Mesh::processMasterBeacon` reads `lastRelayed*`) is untouched.

### 3. #51 — RouteTable pointer allocated on master promotion

**Current** (`firmware/main/src/mesh/Mesh.h:197`): `RouteTable routes;` — inline member allocated on every node. `RouteTable::entries[32]` × 72 bytes = 2304 bytes unconditional.

**New:**

```cpp
// Mesh.h
RouteTable* routes = nullptr;   // owned by Mesh; nullptr on leaves
```

Allocated once at role determination:

```cpp
// Mesh::init (or the setup path that reads isMaster from NVS)
if (isMaster && !routes) {
  routes = new RouteTable();
}
```

Every access site must guard on the pointer. Existing internal call sites are already inside `if (isMaster) { ... }` branches per grep — the guard becomes structural (leaves never reach the code paths that touch `routes`). One accessor for tests:

```cpp
// Mesh.h public (test-only via #ifdef UNIT_TEST)
RouteTable* testRoutes() { return routes; }
```

Callers must handle `nullptr`.

Destruction on master demotion:

```cpp
if (!isMaster && routes) {
  delete routes;
  routes = nullptr;
}
```

Not exercised in production today (role is set at boot and does not change), but the guard is symmetric and cheap.

## Data flow (summary)

`#45` and `#46` do NOT interact — `#45` is upstream of routing decisions, `#46` gates message acceptance. `#51` is orthogonal (memory layout only). All three land in separate commits within one PR.

## Error handling

- `#45`: no new fatal paths; log level `LOG_INFO` on distance change (as existing).
- `#46`: no new fatal paths; drops are silent (existing behaviour).
- `#51`: `new RouteTable()` failure is not handled — ESP-IDF `new` returns `nullptr` on OOM by default, but at 2.3 KB against 279 KB free heap on a bare master this is not a real failure mode. If it ever failed, downstream `routes.record` calls (all guarded by `if (isMaster) ...` outer branches, which now must also add `&& routes`) simply short-circuit. Accept as-is.

## Testing

**Unit (`tests/unit/`):**

- `#45` (`test_neighbor_table.cpp` — extend, or new `test_neighbor_min_fresh.cpp`):
  - `MinFreshDistance_Empty_Returns0xFF`.
  - `MinFreshDistance_SingleFresh_ReturnsIt`.
  - `MinFreshDistance_MultipleFresh_ReturnsMin`.
  - `MinFreshDistance_AllStale_Returns0xFF`.
  - `MinFreshDistance_MixedFreshStale_IgnoresStale`.
- `#45` (`test_mesh_logic.cpp` — extend):
  - `ProcessMasterBeacon_DirectBeacon_DistanceIs1`.
  - `ProcessMasterBeacon_SinglePathAgeOut_DistanceRises` — feed a direct beacon, advance simulated clock past `STALE_PEER_THRESHOLD_MS` for that neighbor, feed a distance-2 beacon via a different relay, assert `currentMaster.distance == 3`.
  - `ProcessMasterBeacon_TwoPathsDifferentLength_NoOscillation` — feed alternating direct + relayed-at-distance-2 beacons; assert distance stays at 1 while both fresh.
- `#46` (`test_replay_cache.cpp` — extend):
  - `IsReplay_FirstFrame_Accepts`.
  - `IsReplay_ExactReplay_Drops`.
  - `IsReplay_StrictlyNewer_Accepts`.
  - `IsReplay_OutOfOrderSameOrigin_Drops`.
  - `IsReplay_DifferentOrigin_DoesNotCollide`.
  - `IsReplay_FullTable_EvictsOldest` — fill `LATTICE_REPLAY_MAX_ORIGINS` distinct origins, add one more, assert oldest by `lastSeenMs` was evicted.
- `#51` (`test_mesh_logic.cpp` — extend or new `test_route_table_allocation.cpp`):
  - `Mesh_LeafRole_RoutesIsNullptr`.
  - `Mesh_MasterPromotion_AllocatesRoutes`.
  - `Mesh_MasterDemotion_FreesRoutes`.

**E2E (`tests/e2e/`):** existing routing and multi-hop scenarios must remain green — no new e2e cases required.

## Non-goals

- No wire-format changes.
- No `NeighborTable::observe` signature change.
- No `RouteTable` API change (only its allocation site).
- No handling of a master-demotion event in a running system — the design supports it symmetrically but nothing currently triggers it.
- Not fixing #47 (hygiene) or #52/#53 (memory-optimisation, Phase G) that touch adjacent code.

## Files touched (estimate)

- `firmware/main/src/mesh/NeighborTable.h` — `+minFreshDistance` (~15 LOC).
- `firmware/main/src/mesh/ReplayCache.h` — full body rewrite (~60 LOC).
- `firmware/main/src/mesh/Mesh.h` — `routes` becomes pointer (~2 LOC).
- `firmware/main/src/mesh/Mesh.cpp` — `processMasterBeacon` distance derivation (~10 LOC); `routes` guards at accessor sites; role-based init (~10 LOC).
- `firmware/main/project_config.h` — `LATTICE_REPLAY_MAX_ORIGINS` knob (~4 LOC).
- Callers of `ReplayCache::isReplay` — pass `millis()`.
- `tests/unit/test_neighbor_table.cpp` — new fixture cases.
- `tests/unit/test_replay_cache.cpp` — extended cases.
- `tests/unit/test_mesh_logic.cpp` — new cases across #45 + #51.

Rough size: ~150 LOC production + ~350 LOC test.
