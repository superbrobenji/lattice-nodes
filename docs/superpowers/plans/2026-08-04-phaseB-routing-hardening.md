# Phase B — Routing hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three Low-severity routing defects — sticky-min `currentMaster.distance` (#45), fixed-ring `ReplayCache` eviction (#46), unconditional `RouteTable` allocation (#51) — per Phase B of the umbrella spec.

**Architecture:** Three isolated components. `#45` swaps sticky-min for `min(fresh-neighbor)+1` derivation in `processMasterBeacon`. `#46` rewrites `ReplayCache` from an exact-tuple ring to a per-origin high-water table with compile-time knob. `#51` converts `RouteTable` from an inline member to a heap pointer allocated only on master promotion. No wire-format changes; no cross-repo work.

**Tech Stack:** C++ (ESP-IDF w/ arduino-esp32 3.3.10), NVS via `Preferences`, GoogleTest+Ctest host unit suite.

## Global Constraints

- No wire-format changes. `mesh_message` layout preserved verbatim.
- Firmware-only. Do not touch `lattice-protocol` or `lattice-hub`.
- Tiger-Style: static alloc after `setup()`, WDT-aware. `RouteTable` heap allocation happens ONCE at role-determination, not on hot path.
- Design doc: `docs/superpowers/specs/2026-08-04-phaseB-routing-hardening-design.md`.
- Parent umbrella: `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase B).
- Under `UNIT_TEST`, `lattice::err::fail` throws `lattice::err::FatalError` (unchanged from Phase A).
- `NeighborTable::observe(mac, masterDistance, nowMillis)` signature unchanged.
- `RouteTable` public API (`.lookup`, `.record`) unchanged; only its allocation site changes.
- `ReplayCache::isReplay(msg)` becomes `ReplayCache::isReplay(msg, nowMs)` — every caller updated.
- New knob `lattice::config::LATTICE_REPLAY_MAX_ORIGINS` (default 16) in `firmware/main/project_config.h`.

---

### Task 1: `#45` — distance from `min(fresh-neighbor) + 1`

**Files:**
- Modify: `firmware/main/src/mesh/NeighborTable.h` — add public `uint8_t minFreshDistance(uint32_t nowMillis) const`.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — rewrite the distance-update block in `processMasterBeacon` (currently at ~lines 756-763; verify with `grep -n "newDistance" firmware/main/src/mesh/Mesh.cpp`).
- Test: `tests/unit/test_neighbor_table.cpp` — 5 new cases for `minFreshDistance`.
- Test: `tests/unit/test_mesh_logic.cpp` — 3 new cases in a new fixture `MeshDistanceDerivationTest`.

**Interfaces:**
- Consumes: existing `NeighborTable::observe`, `STALE_PEER_THRESHOLD_MS`, `LATTICE_NEIGHBOR_MAX`.
- Produces: new `NeighborTable::minFreshDistance(uint32_t)` public const method returning `uint8_t` (`0xFF` = no fresh entry).

- [ ] **Step 1: Write the failing NeighborTable tests**

Append to `tests/unit/test_neighbor_table.cpp` (fixture `NeighborTableTest` exists already):

```cpp
TEST_F(NeighborTableTest, MinFreshDistance_Empty_Returns0xFF) {
  NeighborTable nt;
  EXPECT_EQ(nt.minFreshDistance(1000), 0xFF);
}

TEST_F(NeighborTableTest, MinFreshDistance_SingleFresh_ReturnsIt) {
  NeighborTable nt;
  uint8_t mac[6] = {1,2,3,4,5,6};
  nt.observe(mac, 3, 1000);
  EXPECT_EQ(nt.minFreshDistance(1000), 3);
}

TEST_F(NeighborTableTest, MinFreshDistance_MultipleFresh_ReturnsMin) {
  NeighborTable nt;
  uint8_t m1[6] = {1,0,0,0,0,1};
  uint8_t m2[6] = {1,0,0,0,0,2};
  uint8_t m3[6] = {1,0,0,0,0,3};
  nt.observe(m1, 5, 1000);
  nt.observe(m2, 2, 1000);
  nt.observe(m3, 4, 1000);
  EXPECT_EQ(nt.minFreshDistance(1000), 2);
}

TEST_F(NeighborTableTest, MinFreshDistance_AllStale_Returns0xFF) {
  NeighborTable nt;
  uint8_t mac[6] = {1,2,3,4,5,6};
  nt.observe(mac, 3, 1000);
  uint32_t future = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1;
  EXPECT_EQ(nt.minFreshDistance(future), 0xFF);
}

TEST_F(NeighborTableTest, MinFreshDistance_MixedFreshStale_IgnoresStale) {
  NeighborTable nt;
  uint8_t stale[6] = {1,0,0,0,0,1};
  uint8_t fresh[6] = {1,0,0,0,0,2};
  nt.observe(stale, 1, 1000);
  nt.observe(fresh, 4, 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1);
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 2;
  EXPECT_EQ(nt.minFreshDistance(now), 4);   // stale's 1 ignored
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_neighbor_table --parallel
ctest --test-dir tests/build -R NeighborTableTest.MinFreshDistance --output-on-failure
```

Expected: compile error — `minFreshDistance` not declared.

- [ ] **Step 3: Add `minFreshDistance` to NeighborTable**

In `firmware/main/src/mesh/NeighborTable.h`, inside `class NeighborTable`, public section, after `selectNextHop`:

```cpp
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
```

- [ ] **Step 4: Run NeighborTable tests to verify they pass**

```bash
cmake --build tests/build --target test_neighbor_table --parallel
ctest --test-dir tests/build -R NeighborTableTest.MinFreshDistance --output-on-failure
```

Expected: 5 PASS.

- [ ] **Step 5: Write the failing Mesh distance-derivation tests**

Add to `tests/unit/test_mesh_logic.cpp` a new fixture. It needs a `Mesh` instance with a controllable NeighborTable — use whatever pattern the existing `MeshTest` fixture uses (e.g. `MeshTestHelper` or `friend class`; verify current fixture layout before writing).

```cpp
class MeshDistanceDerivationTest : public ::testing::Test {
protected:
  lattice::mesh::Mesh mesh;
  void SetUp() override {
    // Reset mesh + set device MAC + mark not-master
    resetMillis();
  }
};

TEST_F(MeshDistanceDerivationTest, DirectBeacon_DistanceIs1) {
  // Build a beacon: hop_count=0, last_hop=master, origin=master
  mesh_message m{};
  const uint8_t master[6] = {0xAA,0,0,0,0,1};
  memcpy(m.origin_mac_address, master, 6);
  memcpy(m.last_hop_mac_address, master, 6);
  m.message_type = MESH_TYPE_MASTER_BEACON;
  m.hop_count = 0;
  // Set enrollment.knownMasterMac so TOFU branch accepts
  memcpy(mesh._enrollmentForTest().knownMasterMac, master, 6);
  mesh._enrollmentForTest().hasMasterMac = true;
  mesh.processMasterBeacon(m);
  EXPECT_EQ(mesh._currentMasterForTest().distance, 1);
}

TEST_F(MeshDistanceDerivationTest, SinglePathAgeOut_DistanceRises) {
  // ... first beacon direct → distance=1
  // ... advance clock past STALE_PEER_THRESHOLD_MS
  // ... second beacon via relay-at-distance-2 → distance=3
  // Assert distance == 3
}

TEST_F(MeshDistanceDerivationTest, TwoPathsDifferentLength_NoOscillation) {
  // Interleave direct + relayed beacons while both are fresh
  // Assert distance stays == 1 throughout
}
```

**Note:** if `Mesh` doesn't have test accessors for `enrollment` and `currentMaster`, add them under `#ifdef UNIT_TEST` in `Mesh.h`:

```cpp
#ifdef UNIT_TEST
  Enrollment& _enrollmentForTest() { return enrollment; }
  MasterInfo& _currentMasterForTest() { return currentMaster; }
#endif
```

(Verify at implementation time whether `friend class MeshDistanceDerivationTest;` is more consistent with existing Phase A pattern.)

- [ ] **Step 6: Run Mesh tests to verify they fail**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R MeshDistanceDerivationTest --output-on-failure
```

Expected: compile error or FAIL — old sticky-min still in place.

- [ ] **Step 7: Rewrite distance-update block in `processMasterBeacon`**

In `firmware/main/src/mesh/Mesh.cpp`, replace the block starting at `uint8_t newDistance = msg.hop_count + 1;` (currently ~line 756) up to the closing `}` of the `if` that guards the sticky-min update:

```cpp
  // Derive currentMaster.distance from live NeighborTable state (issue #45).
  // Sticky-min replaced by a pure function of neighbor state: rises monotonically
  // as shorter-path neighbors age out; no oscillation because state can only
  // flap if NeighborTable itself flaps.
  memcpy(currentMaster.mac, msg.origin_mac_address, 6);
  uint8_t min_d = neighbors.minFreshDistance(millis());
  uint8_t derived = (min_d == 0xFF) ? 0xFF : static_cast<uint8_t>(min_d + 1);
  if (derived != currentMaster.distance) {
    currentMaster.distance = derived;
    Logger::logln("MESH", "Route distance derived: " + String(derived), LogLevel::LOG_INFO);
  }
```

Ensure this executes AFTER the existing `neighbors.observe(msg.last_hop_mac_address, msg.hop_count, millis());` call — the derivation depends on the just-observed neighbor being in the table.

- [ ] **Step 8: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_mesh_logic --target test_neighbor_table --parallel
ctest --test-dir tests/build -R "NeighborTableTest.MinFreshDistance|MeshDistanceDerivationTest" --output-on-failure
```

Expected: 8 PASS.

- [ ] **Step 9: Run whole unit suite for regressions**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

Expected: all PASS.

- [ ] **Step 10: Commit**

```bash
git add firmware/main/src/mesh/NeighborTable.h \
        firmware/main/src/mesh/Mesh.cpp \
        firmware/main/src/mesh/Mesh.h \
        tests/unit/test_neighbor_table.cpp \
        tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): derive currentMaster.distance from min(fresh-neighbor)+1

Sticky-min meant a node kept its stale short-path distance after the
shortest relay died, and selectNextHop rejected any surviving longer path
because it required strictly-closer. Uplink dropped for ~9 s until the
master-timeout fallback reset the route.

Replaces the sticky-min in processMasterBeacon with a derivation from
NeighborTable: currentMaster.distance = min(masterDistance over valid
fresh neighbors) + 1, or 0xFF if none fresh. Distance can only flap if
NeighborTable state flaps — one-time monotone rise as shorter-path
neighbors age out. No oscillation.

Adds NeighborTable::minFreshDistance(nowMillis) const.

Part of Phase B (issue #45)."
```

---

### Task 2: `#46` — per-origin high-water `ReplayCache` + knob

**Files:**
- Modify: `firmware/main/project_config.h` — add `LATTICE_REPLAY_MAX_ORIGINS` (default 16).
- Modify: `firmware/main/src/mesh/ReplayCache.h` — rewrite `isReplay`; add per-slot `lastSeenMs` + `used` fields; preserve `bootEpoch`, `txSeqNum`, `nextSeq()`, `lastRelayedEpoch`, `lastRelayedSeqNum`, `init(epoch)` unchanged.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — call-site at line ~367 updated: `replay.isReplay(msg)` → `replay.isReplay(msg, millis())`.
- Test: `tests/unit/test_replay_cache.cpp` — 6 new cases.

**Interfaces:**
- Produces: `bool ReplayCache::isReplay(const mesh_message& msg, uint32_t nowMs)`.
- Consumes: `lattice::config::LATTICE_REPLAY_MAX_ORIGINS` (new knob).

- [ ] **Step 1: Add the knob**

In `firmware/main/project_config.h`, inside `namespace lattice::config`, near other `LATTICE_*_MAX` constants (grep for `LATTICE_NEIGHBOR_MAX` to find the block):

```cpp
// Per-origin ReplayCache slot count (issue #46). Bounds memory to
// LATTICE_REPLAY_MAX_ORIGINS × sizeof(ReplayCache::Entry). Size to
// (expected concurrent origins × 1.5). Default matches the old ring size.
constexpr size_t LATTICE_REPLAY_MAX_ORIGINS = 16;
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/unit/test_replay_cache.cpp`:

```cpp
static mesh_message makeMsg(const uint8_t mac[6], uint32_t epoch, uint16_t seq) {
  mesh_message m{};
  memcpy(m.origin_mac_address, mac, 6);
  m.epoch_num = epoch;
  m.seq_num = seq;
  return m;
}

TEST(ReplayCachePerOrigin, FirstFrame_Accepts) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 1), 1000));
}

TEST(ReplayCachePerOrigin, ExactReplay_Drops) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
}

TEST(ReplayCachePerOrigin, StrictlyNewer_Accepts) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 6), 1001));
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 2, 0), 1002));
}

TEST(ReplayCachePerOrigin, OutOfOrderSameOrigin_Drops) {
  ReplayCache rc; rc.init(1);
  uint8_t mac[6] = {1,2,3,4,5,6};
  EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 5), 1000));
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 1, 4), 1001));   // same epoch, lower seq
  EXPECT_TRUE(rc.isReplay(makeMsg(mac, 0, 999), 1002)); // lower epoch
}

TEST(ReplayCachePerOrigin, DifferentOrigin_DoesNotCollide) {
  ReplayCache rc; rc.init(1);
  uint8_t a[6] = {1,2,3,4,5,6};
  uint8_t b[6] = {6,5,4,3,2,1};
  EXPECT_FALSE(rc.isReplay(makeMsg(a, 1, 5), 1000));
  EXPECT_FALSE(rc.isReplay(makeMsg(b, 1, 5), 1001));   // b's first frame — accept
  EXPECT_TRUE(rc.isReplay(makeMsg(a, 1, 5), 1002));    // a's replay still detected
}

TEST(ReplayCachePerOrigin, FullTable_EvictsOldest) {
  ReplayCache rc; rc.init(1);
  for (size_t i = 0; i < lattice::config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
    uint8_t mac[6] = {static_cast<uint8_t>(i+1), 0, 0, 0, 0, 0};
    EXPECT_FALSE(rc.isReplay(makeMsg(mac, 1, 0), 1000 + i));
  }
  // Table full. Oldest is mac {1,...} with lastSeenMs=1000.
  // Insert one new origin — must evict {1,...}.
  uint8_t newMac[6] = {0xAA, 0, 0, 0, 0, 0};
  EXPECT_FALSE(rc.isReplay(makeMsg(newMac, 1, 0), 2000));
  // Now replay {1,...}'s frame — since its slot was evicted, it looks first-ever.
  uint8_t evicted[6] = {1, 0, 0, 0, 0, 0};
  EXPECT_FALSE(rc.isReplay(makeMsg(evicted, 1, 0), 2001));  // accepted (documented limitation)
}
```

- [ ] **Step 3: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_replay_cache --parallel
ctest --test-dir tests/build -R ReplayCachePerOrigin --output-on-failure
```

Expected: compile error — `isReplay` signature mismatch (needs `nowMs`).

- [ ] **Step 4: Rewrite `ReplayCache`**

Replace `firmware/main/src/mesh/ReplayCache.h` body (preserving header guards, includes, namespace):

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

struct ReplayCache {
  struct Entry {
    uint8_t  mac[6];
    uint32_t epoch;
    uint16_t seq;
    uint32_t lastSeenMs;
    bool     used;
  };

  Entry cache[config::LATTICE_REPLAY_MAX_ORIGINS]{};
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
    memset(cache, 0, sizeof(cache));
  }

  uint16_t nextSeq() { return ++txSeqNum; }

  // Return true if msg is a replay (drop it). Per-origin high-water:
  // accept iff strictly newer (epoch, seq) than the stored tuple.
  // First frame per origin allocates a slot (empty first, else LRU-evict by
  // lastSeenMs). LRU eviction of an active origin lets an attacker who first
  // floods > LATTICE_REPLAY_MAX_ORIGINS distinct origins re-deliver a genuine
  // older frame — AEAD still authenticates content, so worst-case is genuine
  // old delivery, not forgery. Size the knob to expected origins × 1.5.
  inline bool isReplay(const mesh_message& msg, uint32_t nowMs) {
    // 1. Find slot for this origin.
    for (size_t i = 0; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
      if (cache[i].used && memcmp(cache[i].mac, msg.origin_mac_address, 6) == 0) {
        bool newer = (msg.epoch_num > cache[i].epoch) ||
                     (msg.epoch_num == cache[i].epoch && msg.seq_num > cache[i].seq);
        if (!newer) return true;
        cache[i].epoch = msg.epoch_num;
        cache[i].seq = msg.seq_num;
        cache[i].lastSeenMs = nowMs;
        return false;
      }
    }
    // 2. No slot — allocate: first !used, else LRU-evict (smallest lastSeenMs).
    size_t slot = 0;
    bool foundEmpty = false;
    for (size_t i = 0; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
      if (!cache[i].used) { slot = i; foundEmpty = true; break; }
    }
    if (!foundEmpty) {
      slot = 0;
      for (size_t i = 1; i < config::LATTICE_REPLAY_MAX_ORIGINS; ++i) {
        if (cache[i].lastSeenMs < cache[slot].lastSeenMs) slot = i;
      }
    }
    memcpy(cache[slot].mac, msg.origin_mac_address, 6);
    cache[slot].epoch = msg.epoch_num;
    cache[slot].seq = msg.seq_num;
    cache[slot].lastSeenMs = nowMs;
    cache[slot].used = true;
    return false;
  }
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 5: Update the sole production caller**

In `firmware/main/src/mesh/Mesh.cpp` (~line 367):

```cpp
if (replay.isReplay(msg, millis())) {
```

- [ ] **Step 6: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_replay_cache --target test_mesh_logic --parallel
ctest --test-dir tests/build -R "ReplayCache|MeshTest" --output-on-failure
```

Expected: all new + existing tests PASS. Existing `test_replay_cache.cpp` cases that used the old signature will fail to compile — update them to pass a `nowMs` argument (e.g. `1000`), verify the pre-existing assertions still hold under the new per-origin semantics; if a pre-existing case asserted the old ring's round-robin eviction behaviour, its assertion needs updating to the new per-origin semantics (do NOT weaken; ensure the new semantics still bar the old attack the pre-existing test was written to catch).

- [ ] **Step 7: Full unit suite regression**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 8: Commit**

```bash
git add firmware/main/project_config.h \
        firmware/main/src/mesh/ReplayCache.h \
        firmware/main/src/mesh/Mesh.cpp \
        tests/unit/test_replay_cache.cpp
git commit -m "feat(mesh): per-origin high-water ReplayCache (closes eviction window)

Old 16-entry round-robin ring evicted still-relevant (origin, epoch, seq)
entries in dense meshes, letting a replayed genuine older frame slip
through once its slot was overwritten.

Replaces the ring with a per-origin table: one slot per origin MAC,
storing the highest-seen (epoch, seq). Accept iff strictly newer.
Memory bounded by peer count (LATTICE_REPLAY_MAX_ORIGINS × 21 B), not
message rate. LRU eviction of an origin slot on overflow — same
degrade-not-fail bound as before, but tighter per-origin guarantee
below the eviction threshold. AEAD still authenticates content, so
worst case remains re-delivery of a genuine old frame, not forgery.

Adds LATTICE_REPLAY_MAX_ORIGINS knob (default 16).
isReplay(msg) becomes isReplay(msg, nowMs).

Part of Phase B (issue #46)."
```

---

### Task 3: `#51` — `RouteTable` pointer allocated on master promotion

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h` — `RouteTable routes;` → `RouteTable* routes = nullptr;`. Update `testRoutes()` return type. Add destructor.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — allocate in `Mesh::init` when `isMaster`; free in destructor and on demotion; guard both call sites (`Mesh.cpp:624` `.lookup`, `Mesh.cpp:1126` `.record`).
- Modify: `tests/unit/test_mesh_logic.cpp:881` — adapt to `testRoutes()` returning a pointer.
- Modify: `tests/unit/test_route_report.cpp:295` — adapt.
- Modify: `tests/e2e/scenarios/test_route_report_e2e.cpp:77` — adapt.
- Test: `tests/unit/test_mesh_logic.cpp` — 3 new cases in fixture `MeshRouteTableAllocationTest`.

**Interfaces:**
- Consumes: `Mesh::isMaster` bool member (set via `setIsMaster` before `init`).
- Produces: `RouteTable* Mesh::routes` (was reference `RouteTable& Mesh::routes`). All callers must handle nullptr — either explicit guard, or the caller is already inside an `if (isMaster) { ... }` outer branch.
- Produces: `RouteTable* Mesh::testRoutes()` (was `RouteTable&`). Test callers dereference or handle nullptr.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_mesh_logic.cpp`:

```cpp
class MeshRouteTableAllocationTest : public ::testing::Test {
protected:
  void SetUp() override { resetMillis(); }
};

TEST_F(MeshRouteTableAllocationTest, LeafRole_RoutesIsNullptr) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(false);
  mesh.init();  // or whatever sequence brings the mesh to a live state
  EXPECT_EQ(mesh.testRoutes(), nullptr);
}

TEST_F(MeshRouteTableAllocationTest, MasterPromotion_AllocatesRoutes) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(true);
  mesh.init();
  EXPECT_NE(mesh.testRoutes(), nullptr);
}

TEST_F(MeshRouteTableAllocationTest, MasterDemotion_FreesRoutes) {
  lattice::mesh::Mesh mesh;
  mesh.setIsMaster(true);
  mesh.init();
  ASSERT_NE(mesh.testRoutes(), nullptr);
  mesh.setIsMaster(false);
  mesh.reevaluateRouteTable();  // new helper: honours current isMaster state
  EXPECT_EQ(mesh.testRoutes(), nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R MeshRouteTableAllocationTest --output-on-failure
```

Expected: compile error — `routes` is currently a reference/inline member.

- [ ] **Step 3: Change `routes` to pointer + add destructor + helper**

In `firmware/main/src/mesh/Mesh.h`:

- Replace `RouteTable routes;` (~line 197) with:
  ```cpp
  RouteTable* routes = nullptr;   // allocated only when this node is a master (issue #51)
  ```
- Replace `RouteTable& testRoutes() { return routes; }` (~line 155) with:
  ```cpp
  RouteTable* testRoutes() { return routes; }
  ```
- Add near other public methods:
  ```cpp
  ~Mesh() { delete routes; routes = nullptr; }
  // Re-evaluate whether this node needs a RouteTable, honouring the current
  // isMaster flag. Called from init() and on live role changes.
  void reevaluateRouteTable() {
    if (isMaster && !routes) routes = new RouteTable();
    if (!isMaster && routes) { delete routes; routes = nullptr; }
  }
  ```

Note: `Mesh` may already have a destructor — if so, add the `delete routes` line rather than a new destructor.

- [ ] **Step 4: Call the helper from `Mesh::init`**

In `firmware/main/src/mesh/Mesh.cpp`, in `Mesh::init()` (near the top, right after `loadPersistentState()`):

```cpp
  reevaluateRouteTable();
```

- [ ] **Step 5: Guard the two production `routes.` call-sites**

At `firmware/main/src/mesh/Mesh.cpp:624` — currently `if (routes.lookup(destMac, path, &pathLen) && pathLen > 0)`. Change to:

```cpp
  if (routes && routes->lookup(destMac, path, &pathLen) && pathLen > 0) {
```

At `firmware/main/src/mesh/Mesh.cpp:1126` — currently `routes.record(msg.origin_mac_address, msg.route_path, msg.route_len, millis());`. Change to:

```cpp
    if (routes) {
      routes->record(msg.origin_mac_address, msg.route_path, msg.route_len, millis());
    }
```

Verify with a repo-wide grep that no other `routes.` reference exists in production code:

```bash
grep -rn "routes\." firmware/main/src/
```

- [ ] **Step 6: Update test-file callers of `testRoutes()`**

Three test files use `mesh.testRoutes().lookup(...)` or `.record(...)`. All three now need to either:

- Master-mode tests: `mesh.setIsMaster(true); mesh.init();` (or equivalent existing setup) so `testRoutes()` is non-null; then `mesh.testRoutes()->lookup(...)` / `->record(...)`.
- Leaf-mode tests: guard on `ASSERT_NE(mesh.testRoutes(), nullptr) << "test expects master allocation";` before calling, since the previous inline reference never null.

Files:
- `tests/unit/test_mesh_logic.cpp:881` — `master.testRoutes().record(...)` → `ASSERT_NE(master.testRoutes(), nullptr); master.testRoutes()->record(...);` (or ensure `master.setIsMaster(true)` upstream in fixture SetUp).
- `tests/unit/test_route_report.cpp:295` — `mesh.testRoutes().lookup(...)` → same treatment.
- `tests/e2e/scenarios/test_route_report_e2e.cpp:77` — `m.testRoutes().lookup(...)` → same treatment.

- [ ] **Step 7: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_mesh_logic --target test_route_report --parallel
ctest --test-dir tests/build -R "MeshRouteTableAllocationTest|RouteReport" --output-on-failure
```

Expected: 3 new PASS, plus adapted existing tests PASS.

- [ ] **Step 8: Full unit suite regression**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/Mesh.h \
        firmware/main/src/mesh/Mesh.cpp \
        tests/unit/test_mesh_logic.cpp \
        tests/unit/test_route_report.cpp \
        tests/e2e/scenarios/test_route_report_e2e.cpp
git commit -m "feat(mesh): allocate RouteTable only on master promotion

RouteTable was an inline member of Mesh, so every node paid ~2.25 KB
static RAM (32 entries × 72 B) even though only masters use it.

Converts to a pointer allocated in Mesh::init() when isMaster is set;
freed in the destructor and on live role change via
reevaluateRouteTable(). Leaves allocate zero. Both production call
sites (Mesh.cpp lookup, record) now guarded; testRoutes() returns a
pointer.

Part of Phase B (issue #51)."
```

---

### Task 4: E2E verification + PR

**Files:**
- No production changes.
- Modify: `.superpowers/sdd/phaseB-routing-hardening/progress.md` — new SDD ledger file (gitignored).

**Interfaces:**
- Consumes: unit + e2e test binaries.
- Produces: PR.

- [ ] **Step 1: Full host suite green**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

Expected: 0 failures.

- [ ] **Step 2: E2E suite green (route + multi-hop paths unchanged in behaviour)**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 -L e2e -R "route|multihop|beacon"
```

Expected: all PASS. Pre-existing DualMasterTest failures (documented in Phase 0) still excluded.

- [ ] **Step 3: Write SDD ledger**

Create `.superpowers/sdd/phaseB-routing-hardening/progress.md`:

```markdown
# SDD ledger — plan: docs/superpowers/plans/2026-08-04-phaseB-routing-hardening.md
Task 1: complete (commit <sha>) — #45 distance derivation
Task 2: complete (commit <sha>) — #46 per-origin high-water ReplayCache
Task 3: complete (commit <sha>) — #51 RouteTable pointer
Task 4: complete — full suite green
```

- [ ] **Step 4: Push branch + open PR**

```bash
git push -u origin feat/phaseB-routing-hardening
gh pr create --title "feat(phaseB): routing hardening (closes #45, #46, #51)" \
             --body "$(cat <<'EOF'
Implements docs/superpowers/plans/2026-08-04-phaseB-routing-hardening.md.

Closes #45, #46, #51.

## Summary
- #45 currentMaster.distance derived from min(fresh-neighbor)+1 (no oscillation).
- #46 ReplayCache per-origin high-water + LATTICE_REPLAY_MAX_ORIGINS knob.
- #51 RouteTable allocated only on master (~2.25 KB saved per leaf).

## Test plan
- [x] Host unit suite green.
- [x] E2E route+multi-hop scenarios green.
- [ ] CI green.
EOF
)"
```

---

## Self-review

**Spec coverage:**
- §Design/1 (#45 distance) → Task 1.
- §Design/2 (#46 per-origin) → Task 2.
- §Design/3 (#51 pointer) → Task 3.
- §Testing (unit + e2e regression) → Task 1 (5 NT + 3 Mesh), Task 2 (6 replay), Task 3 (3 allocation), Task 4 (regression).
- §Non-goals — respected: no wire changes, no cross-repo, no #47/#52/#53 touches.

**Type consistency:**
- `NeighborTable::minFreshDistance(uint32_t nowMillis) const → uint8_t` — declared Task 1 Step 3, consumed Task 1 Step 7.
- `ReplayCache::isReplay(const mesh_message&, uint32_t nowMs) → bool` — declared Task 2 Step 4, consumed Task 2 Step 5 and by the Mesh caller Step 5.
- `Mesh::routes: RouteTable*` (was `RouteTable`) — declared Task 3 Step 3, consumed Task 3 Steps 4-6.
- `Mesh::testRoutes() → RouteTable*` (was `RouteTable&`) — declared Task 3 Step 3, consumed Task 3 Step 6.
- `Mesh::reevaluateRouteTable() → void` — declared Task 3 Step 3, consumed Task 3 Step 4 and Task 3 test Step 1.
- `lattice::config::LATTICE_REPLAY_MAX_ORIGINS: size_t` — declared Task 2 Step 1, consumed Task 2 Steps 2 and 4.

**Placeholder scan:** none. One hedge — Task 1 Step 5 says "verify current fixture layout before writing" and "verify at implementation time whether `friend class` is more consistent". Both are legitimate implementer-time judgement calls with concrete alternatives named.

**Scope check:** three tasks + verify task, all in one repo, one design doc, single PR — appropriate for one plan. Task 1 has a large step count (10) because it touches two components (NT + Mesh) via TDD; each step is still bite-sized.
