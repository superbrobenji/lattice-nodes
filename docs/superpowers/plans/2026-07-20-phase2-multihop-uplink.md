<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phase 2: NeighborTable + Multi-Hop Data Uplink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A sensor node at hop-distance ≥ 2 from the master can uplink sealed adapter data through an intermediate relay, closing design gap #7.

**Architecture:** A new RAM-only `NeighborTable` records forwarding candidates learned from overheard master beacons (`mac`, `masterDistance`, `lastSeenMillis`). `findNextHopToMaster()` stops requiring the next hop to be an enrolled `PeerRegistry` peer and instead selects the freshest neighbor whose distance to the master is strictly less than the node's own, auto-registering that neighbor as an unencrypted ESP-NOW peer on first forward. Relays already forward sealed ciphertext they cannot read (Phase 1), so no crypto changes are needed — this is pure routing plumbing.

**Tech Stack:** C++ (ESP32 firmware, host-tested), GoogleTest + `tests/e2e` sim harness (VirtualBus / FakeHub / MeshSimTest). `main/lib/lattice-protocol` submodule at v0.4.0 (proto v3, unchanged this phase).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-16-multihop-routing-e2e-crypto-design.md` §3 (uplink routing — NeighborTable); §2 trust-split (NeighborTable holds routing only, never key material).
- **NeighborTable entry:** `mac(6)`, `masterDistance(u8)`, `lastSeenMillis(u32)`. Populated from overheard master beacons — a relayed beacon's `msg.hop_count + 1` is the *neighbor's* distance to the master, and `msg.last_hop_mac_address` is the neighbor's MAC.
- **Next-hop selection:** freshest neighbor with `masterDistance` **strictly less than** the node's own `currentMaster.distance` (strict `<` prevents equal-distance siblings ping-ponging a frame). "Freshest" = smallest `millis() - lastSeenMillis` among the eligible.
- **Staleness:** reuse `STALE_PEER_THRESHOLD_MS` (8000 ms, `project_config.h:122`) — a neighbor is eligible only if `millis() - lastSeenMillis < STALE_PEER_THRESHOLD_MS`.
- **Eviction when full:** evict a stale entry first; if none stale, evict the entry with the largest `masterDistance`.
- **Size:** compile-time `LATTICE_NEIGHBOR_MAX` in `lattice::config` (spec default **8**). Does not exist yet — add it.
- The chosen next-hop neighbor is auto-added as an **unencrypted** ESP-NOW peer via the existing single-arg `lattice::mesh::crypto::registerPeerWithEspNow(const uint8_t* mac)` (`MeshCrypto.h:19`) before the frame is sent to it.
- **NeighborTable never holds key material** and is never consulted for E2E keys — E2E keys stay a (node, master) pair concept (`masterE2EKeys`/`peerE2EKeys`, unchanged).
- **PeerRegistry's enrollment-only add rule is unchanged** (`PeerRegistry.cpp` `updateLastSeen` comment). NeighborTable is a *separate* structure; do not weaken PeerRegistry.
- `MAX_HOPS = 10` (`project_config.h:118`) bounds relay depth (existing check in the relay branches).
- Relays forward sealed ciphertext untouched (Phase 1) — this phase must not read/modify `msg.data` in any relay path.
- All firmware test hooks `#ifdef UNIT_TEST`-gated (PR #28 convention).
- **clang-format 18** (not local Homebrew v22) — CI `lint-format` runs Ubuntu apt clang-format 18. Before every commit run the dry-run with v18: `python3 -m pip install 'clang-format==18.1.8'` once, then `CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")` and check with `find main/src \( -name '*.cpp' -o -name '*.h' \) ! -path '*/nanopb/*' ! -name 'mesh.pb.h' ! -name 'mesh.pb.c' | xargs "$CF" --style=file --dry-run --Werror`. The local `ctest` loop does NOT run clang-format.
- **CodeQL:** no raw arrays in interfaces — function parameters take `const uint8_t* mac`, never `const uint8_t mac[6]` (matches `PeerRegistry::find`). Alert #300 class.
- Verification loop: `cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release` (once), then `cmake --build tests/build --parallel` and `ctest --test-dir tests/build --output-on-failure`. Baseline is **147 pass / 2 disabled**. No device build in CI.

## Current-state facts (verified on this branch)

- `findNextHopToMaster()` — `Mesh.cpp:79-92`: returns `nullptr` if `currentMaster.distance == 0xFF`; else scans `peers` (enrollment-only registry) for a MAC == `currentMaster.nextHop` that is in-range and not self. **This is the gap:** an un-enrolled relay is never in `peers`, so distance-2 leaves get `nullptr`.
- `currentMaster` is a `MasterInfo{ uint8_t mac[6]; uint8_t distance; uint8_t nextHop[6]; }` (`PeerRegistry.h:21-25`); `.distance == 0xFF` means "no master". Set in `processMasterBeacon` (`Mesh.cpp:607-617`): `newDistance = msg.hop_count + 1`, `nextHop = msg.last_hop_mac_address`.
- `processMasterBeacon()` — `Mesh.cpp:544-642`. Beacon relay dedup via `replay.lastRelayedEpoch/lastRelayedSeqNum`; deferred jitter re-broadcast via `relayPendingMsg/relayPendingAt/relayPending` dispatched in `loop()`.
- `transmitCore()` — `Mesh.cpp:356-398`: seals self-originated uplink (Phase 1), then `nextHop = findNextHopToMaster()`; sends if non-null else logs+drops.
- `registerPeerWithEspNow(const uint8_t* mac)` — `MeshCrypto.h:19-28`: single-arg, `encrypt=false`, guarded by `esp_now_is_peer_exist`.
- `MacAddress` compare helper: `lattice::utils::MacAddress(a) == lattice::utils::MacAddress(b)` (used throughout `Mesh.cpp`).
- Sim caveat: `VirtualBus` does NOT enforce ESP-NOW peer registration (unicast to any linked SimNode delivers regardless). So a passing e2e test does not by itself prove the firmware registered the relay peer — Task 4 adds a unit test that asserts `registerPeerWithEspNow` is invoked for the selected next hop.
- The disabled test's block comment (`test_multihop_e2e.cpp:66-82`) is stale (mentions `err::fail` no longer present) — Task 5 updates it.

## File Structure

- **Create** `main/src/mesh/NeighborTable.h` — the routing table (header-only, like `E2EKeyStore.h`): struct + fixed array + observe/select/evict logic. One responsibility: track beacon-learned forwarding candidates and pick the best next hop toward the master.
- **Create** `tests/unit/test_neighbor_table.cpp` — unit tests for observe/select/evict/staleness.
- **Modify** `main/project_config.h` — add `LATTICE_NEIGHBOR_MAX`.
- **Modify** `main/src/mesh/Mesh.h` — add `NeighborTable neighbors;` member + a `#ifdef UNIT_TEST` accessor; declare no new public API.
- **Modify** `main/src/mesh/Mesh.cpp` — feed beacons into the table (`processMasterBeacon`); rewrite `findNextHopToMaster()` to consult the table + auto-register the chosen peer.
- **Modify** `tests/CMakeLists.txt` — register the new unit test.
- **Modify** `tests/e2e/scenarios/test_multihop_e2e.cpp` — enable the disabled test, refresh the stale comment.

---

### Task 1: `LATTICE_NEIGHBOR_MAX` config knob

**Files:**
- Modify: `main/project_config.h` (near `LATTICE_E2E_KEYCACHE_MAX`, ~line 116)

**Interfaces:**
- Produces: `lattice::config::LATTICE_NEIGHBOR_MAX` (`size_t`, value 8).

- [ ] **Step 1: Add the knob**

In `main/project_config.h`, immediately after the `LATTICE_E2E_KEYCACHE_MAX` line, add:

```cpp
// Multi-hop uplink routing (spec §3): max beacon-learned forwarding neighbors
// tracked per node. One entry per distinct upstream relay a node can hear.
inline constexpr size_t LATTICE_NEIGHBOR_MAX = 8;
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build tests/build --parallel`
Expected: builds clean (the constant is unused so far — a `-Wunused` warning is not emitted for `inline constexpr` at namespace scope; if the build is clean, proceed).

- [ ] **Step 3: clang-format check + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/project_config.h
git add main/project_config.h
git commit -m "feat: add LATTICE_NEIGHBOR_MAX config knob for multi-hop routing"
```

(If clang-format isn't installed: `python3 -m pip install 'clang-format==18.1.8'` first.)

---

### Task 2: NeighborTable — observe + select + evict (unit-tested in isolation)

**Files:**
- Create: `main/src/mesh/NeighborTable.h`
- Create: `tests/unit/test_neighbor_table.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `lattice::config::LATTICE_NEIGHBOR_MAX` (Task 1); `STALE_PEER_THRESHOLD_MS` (`project_config.h`).
- Produces: `class lattice::mesh::NeighborTable` with:
  - `void observe(const uint8_t* mac, uint8_t masterDistance, uint32_t nowMillis)` — insert/update the neighbor's distance + lastSeen; on a full table with no existing entry for `mac`, evict per policy.
  - `bool selectNextHop(uint8_t ownDistance, uint32_t nowMillis, uint8_t* outMac)` — write the freshest in-range neighbor with `masterDistance < ownDistance` into `outMac[6]`, return `true`; return `false` if none eligible.
  - `bool contains(const uint8_t* mac) const` — true if a valid entry for `mac` exists (used to assert eviction crisply).
  - `void clear()`.
  - Non-copyable (matches `E2EKeyStore`).
- `nowMillis` is passed in (not read from `millis()` inside) so the table is unit-testable on host without the time mock — the caller in `Mesh.cpp` passes `millis()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_neighbor_table.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/NeighborTable.h"

using namespace lattice::mesh;

static const uint8_t A[6] = {0x02, 0, 0, 0, 0, 0xA1};
static const uint8_t B[6] = {0x02, 0, 0, 0, 0, 0xB2};
static const uint8_t C[6] = {0x02, 0, 0, 0, 0, 0xC3};

TEST(NeighborTable, SelectsCloserNeighbor) {
  NeighborTable t;
  t.observe(A, 1, 1000); // A is 1 hop from master
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 1000, out)); // own distance 2
  EXPECT_EQ(0, memcmp(out, A, 6));
}

TEST(NeighborTable, RejectsEqualOrFartherNeighbor) {
  NeighborTable t;
  t.observe(A, 2, 1000); // same distance as us
  t.observe(B, 3, 1000); // farther
  uint8_t out[6];
  EXPECT_FALSE(t.selectNextHop(2, 1000, out)); // strict < required
}

TEST(NeighborTable, PicksFreshestAmongEligible) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  t.observe(B, 1, 5000); // same distance, seen more recently
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 5000, out));
  EXPECT_EQ(0, memcmp(out, B, 6)); // freshest wins
}

TEST(NeighborTable, StaleNeighborNotEligible) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  uint8_t out[6];
  // now is 1000 + STALE + 1 → A is stale
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1;
  EXPECT_FALSE(t.selectNextHop(2, now, out));
}

TEST(NeighborTable, ObserveUpdatesExistingEntry) {
  NeighborTable t;
  t.observe(A, 3, 1000);
  t.observe(A, 1, 2000); // same MAC, better distance + newer
  uint8_t out[6];
  ASSERT_TRUE(t.selectNextHop(2, 2000, out));
  EXPECT_EQ(0, memcmp(out, A, 6));
}

TEST(NeighborTable, EvictsFarthestWhenFullAndNoneStale) {
  NeighborTable t;
  // Fill all slots, all fresh (t=1000), distances 2..(MAX+1) so the farthest is
  // uniquely identifiable. Slot i → mac {..,i}, distance i+2.
  uint8_t farthest[6] = {0x02, 0, 0, 0, 0,
                         static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 0; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    t.observe(mac, static_cast<uint8_t>(i + 2), 1000);
  }
  ASSERT_TRUE(t.contains(farthest)); // the largest-distance entry, before eviction
  // Table full, nothing stale → inserting C must evict the farthest entry.
  t.observe(C, 1, 1000);
  EXPECT_TRUE(t.contains(C)) << "new neighbor inserted";
  EXPECT_FALSE(t.contains(farthest)) << "farthest-from-master entry evicted";
}

TEST(NeighborTable, EvictsStaleBeforeFarthest) {
  NeighborTable t;
  // slot 0: a CLOSE neighbor (distance 2) observed long ago → will be stale.
  // slots 1..MAX-1: FARTHER neighbors (distance 6+) observed recently → fresh.
  // Inserting C must evict the stale close one, NOT the fresh farthest one.
  uint8_t stale[6] = {0x02, 0, 0, 0, 0, 0x00};
  t.observe(stale, 2, 1000); // old
  uint8_t freshFarthest[6] = {0x02, 0, 0, 0, 0,
                              static_cast<uint8_t>(lattice::config::LATTICE_NEIGHBOR_MAX - 1)};
  for (size_t i = 1; i < lattice::config::LATTICE_NEIGHBOR_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    t.observe(mac, static_cast<uint8_t>(i + 5), 6000); // farther, observed recently
  }
  uint32_t now = 1000 + lattice::config::STALE_PEER_THRESHOLD_MS + 1; // 9001
  // At now: stale (age 8001) is stale; the t=6000 entries (age 3001) are fresh.
  t.observe(C, 1, now);
  EXPECT_TRUE(t.contains(C));
  EXPECT_FALSE(t.contains(stale)) << "stale entry evicted first";
  EXPECT_TRUE(t.contains(freshFarthest)) << "fresh farthest entry survives — stale beats farthest";
}

TEST(NeighborTable, ClearEmptiesTable) {
  NeighborTable t;
  t.observe(A, 1, 1000);
  t.clear();
  uint8_t out[6];
  EXPECT_FALSE(t.selectNextHop(2, 1000, out));
}
```

Add to `tests/CMakeLists.txt` next to the other `add_unit_test` calls (mirror the exact macro form, e.g. the `test_e2e_keystore` line added in Phase 1):

```cmake
add_unit_test(test_neighbor_table unit/test_neighbor_table.cpp)
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --target test_neighbor_table --parallel
```
Expected: FAIL — `NeighborTable.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `main/src/mesh/NeighborTable.h`:

```cpp
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
// A neighbor's masterDistance is (beacon.hop_count + 1) of the best beacon heard
// from it; its mac is that beacon's last_hop_mac_address. Next hop = freshest
// neighbor strictly closer to the master than we are.
class NeighborTable {
public:
  NeighborTable() = default;
  NeighborTable(const NeighborTable&) = delete;
  NeighborTable& operator=(const NeighborTable&) = delete;

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

  void clear() {
    memset(entries, 0, sizeof(entries));
  }

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
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_neighbor_table --parallel && ctest --test-dir tests/build -R NeighborTable --output-on-failure
```
Expected: 8 tests PASS.

- [ ] **Step 5: clang-format + full suite + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/NeighborTable.h tests/unit/test_neighbor_table.cpp
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
git add main/src/mesh/NeighborTable.h tests/unit/test_neighbor_table.cpp tests/CMakeLists.txt
git commit -m "feat: NeighborTable — beacon-learned multi-hop forwarding candidates"
```
Expected: full suite 155 pass / 2 disabled (147 baseline + 8 new).

---

### Task 3: Feed beacons into the NeighborTable

**Files:**
- Modify: `main/src/mesh/Mesh.h` (add member + UNIT_TEST accessor)
- Modify: `main/src/mesh/Mesh.cpp` (`processMasterBeacon`)

**Interfaces:**
- Consumes: `NeighborTable::observe` (Task 2).
- Produces: `Mesh::neighbors` member populated from every accepted master beacon. Adds `#ifdef UNIT_TEST NeighborTable& testNeighbors()`.

- [ ] **Step 1: Add the member and include**

In `main/src/mesh/Mesh.h`: add `#include "NeighborTable.h"` next to the other mesh includes; add a private member alongside `e2eKeys` (search for `E2EKeyStore e2eKeys;`):

```cpp
NeighborTable neighbors;
```

Add a test accessor next to the existing `#ifdef UNIT_TEST` hooks (search for `testReplay`):

```cpp
#ifdef UNIT_TEST
  NeighborTable& testNeighbors() { return neighbors; }
  uint32_t testMillisNow() { return millis(); } // exposes the node's mocked clock to tests
#endif
```

(`millis()` here is the same mocked host clock the sim advances — both Task 3 and Task 4 tests use `testMillisNow()` to pass a `nowMillis` consistent with what the firmware sees.)

- [ ] **Step 2: Write the failing test**

Add to `tests/e2e/scenarios/test_multihop_e2e.cpp` (a new enabled test that checks the table is populated — this also exercises the real beacon path). Mirror the fixture idiom already in that file (`addMaster`, `addSensor`, `world.bus.link`, `enroll`, `runPolled`, `SimNode::with`):

```cpp
TEST_F(MeshSimTest, RelayLearnsNeighborFromMasterBeacon) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  world.bus.link(master, relay);
  enroll(relay);
  runPolled(4000); // let >=1 master beacon (3s interval) reach the relay

  bool eligible = false;
  relay->with([&](lattice::mesh::Mesh& m, auto*) {
    uint8_t out[6];
    // relay is distance 1 from master; a distance-0 neighbor (the master) must
    // be selectable as next hop for a hypothetical distance-1 sender.
    eligible = m.testNeighbors().selectNextHop(1, m.testMillisNow(), out);
    if (eligible)
      EXPECT_EQ(memcmp(out, master->mac(), 6), 0) << "master is the distance-0 neighbor";
  });
  EXPECT_TRUE(eligible) << "relay should have learned the master as a neighbor from its beacon";
}
```

(`testMillisNow()` was added in Step 1; it returns the same mocked clock the node sees.)

- [ ] **Step 3: Run to verify it fails**

```bash
cmake --build tests/build --target lattice_e2e --parallel && ctest --test-dir tests/build -R RelayLearnsNeighborFromMasterBeacon --output-on-failure
```
Expected: FAIL — `selectNextHop` returns false (beacons not yet fed into the table).

- [ ] **Step 4: Feed beacons into the table**

In `main/src/mesh/Mesh.cpp` `processMasterBeacon` (~`Mesh.cpp:544-642`): after the frame has passed its validity/dedup checks and the sender is confirmed a legitimate beacon (i.e. at the same point `currentMaster` is updated, right after the `currentMaster.distance`/`nextHop` assignment block at ~`:607-617`), record the neighbor the beacon was heard through:

```cpp
// Multi-hop routing (spec §3): the node we heard this beacon THROUGH
// (last_hop) is a forwarding candidate; its distance to the master is this
// beacon's hop_count + 1. Learned here, not from enrollment — routing only.
neighbors.observe(msg.last_hop_mac_address, msg.hop_count + 1, millis());
```

Place this so it runs for every accepted beacon (including relayed ones with `hop_count > 0`), not only when `currentMaster` changes — a node needs all its upstream options, not just the current best. If the `currentMaster` update is inside a conditional, put the `neighbors.observe(...)` call OUTSIDE that conditional but still after the dedup/validity gate.

- [ ] **Step 5: Run test + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: `RelayLearnsNeighborFromMasterBeacon` PASSES; all prior tests still green (156 pass / 2 disabled).

- [ ] **Step 6: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/e2e/scenarios/test_multihop_e2e.cpp
git add main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/e2e/scenarios/test_multihop_e2e.cpp
git commit -m "feat: populate NeighborTable from master beacons"
```

---

### Task 4: Route through the NeighborTable + auto-register next hop

**Files:**
- Modify: `main/src/mesh/Mesh.cpp` (`findNextHopToMaster`)
- Modify: `tests/unit/test_mesh_logic.cpp` (unit test that next hop is registered as ESP-NOW peer)

**Interfaces:**
- Consumes: `NeighborTable::selectNextHop` (Task 2); `crypto::registerPeerWithEspNow` (`MeshCrypto.h:19`); `peers.find` (`PeerRegistry`).
- Produces: `findNextHopToMaster()` returns a routable next hop even when it is an un-enrolled relay, auto-registering it as an unencrypted ESP-NOW peer. Signature is unchanged (`PeerInfo* findNextHopToMaster()`).

**Design note on the return type:** `findNextHopToMaster()` currently returns `PeerInfo*` into `peers`. A pure relay is not in `peers` and must not be added there (enrollment-only rule). To keep `transmitCore`'s call site (`sendMessage(nextHop->mac, msg)`) working without restructuring, return a pointer to a small owned `PeerInfo` scratch buffer holding just the resolved MAC. This keeps the change local. The scratch buffer is a `Mesh` member so the returned pointer stays valid until the next call.

- [ ] **Step 1: Write the failing unit test**

Add to `tests/unit/test_mesh_logic.cpp` (mirror the existing `JoinAckRelayTest`/`makeIntermediateNode` idiom — read the file for the fixture and how `currentMaster`/`neighbors` are seeded and how the esp_now mock records peers):

```cpp
TEST_F(JoinAckRelayTest, NextHopThroughRelayIsRegisteredAsEspNowPeer) {
  Mesh mesh = makeIntermediateNode(); // distance/enrollment set up by fixture
  // Node is distance 2; a relay at distance 1 is known ONLY via the NeighborTable
  // (never enrolled → never in PeerRegistry).
  const uint8_t relayMac[6] = {0x02, 0, 0, 0, 0, 0x77};
  mesh.currentMaster.distance = 2;
  mesh.testNeighbors().observe(relayMac, 1, mesh.testMillisNow());

  resetEspNowMock(); // clear recorded peers (mirror the mock's reset used elsewhere)
  PeerInfo* hop = mesh.findNextHopToMaster();

  ASSERT_NE(hop, nullptr) << "distance-2 node must route through the distance-1 relay";
  EXPECT_EQ(memcmp(hop->mac, relayMac, 6), 0);
  EXPECT_TRUE(espNowPeerExists(relayMac)) << "relay must be auto-registered as an ESP-NOW peer";
}
```

Use whatever the esp_now mock actually exposes to assert registration — grep `tests/mocks/esp_now_mock.*` for the recorded-peer query (e.g. `esp_now_is_peer_exist` is already mockable; if there's no `espNowPeerExists` helper, assert via `esp_now_is_peer_exist(relayMac)` directly, which the mock implements). Match the reset helper the other tests use; if none, the mock's peer list may need an explicit reset call the fixture already performs — check `SetUp()`.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build tests/build --target test_mesh_logic --parallel && ctest --test-dir tests/build -R NextHopThroughRelayIsRegisteredAsEspNowPeer --output-on-failure
```
Expected: FAIL — current `findNextHopToMaster` returns `nullptr` (relay not in `peers`).

- [ ] **Step 3: Add the scratch member**

In `main/src/mesh/Mesh.h`, next to `NeighborTable neighbors;`, add a private scratch buffer:

```cpp
PeerInfo nextHopScratch{}; // holds a NeighborTable-resolved next hop (not an enrolled peer)
```

- [ ] **Step 4: Rewrite `findNextHopToMaster`**

Replace the body of `findNextHopToMaster()` (`Mesh.cpp:79-92`) with:

```cpp
PeerInfo* Mesh::findNextHopToMaster() {
  if (currentMaster.distance == 0xFF)
    return nullptr;

  // Prefer an enrolled peer that is the direct master and in range (distance 1,
  // the common single-hop case) — keeps the existing behavior and E2E peering.
  PeerInfo* direct = peers.find(currentMaster.mac);
  if (direct && currentMaster.distance == 1 && peers.isPeerInRange(direct->mac) &&
      lattice::utils::MacAddress(direct->mac) != lattice::utils::MacAddress(deviceMacAddress))
    return direct;

  // Multi-hop (spec §3): pick the freshest neighbor strictly closer to the
  // master from the NeighborTable. The relay need not be an enrolled peer.
  uint8_t hopMac[6];
  if (!neighbors.selectNextHop(currentMaster.distance, millis(), hopMac))
    return nullptr;
  if (lattice::utils::MacAddress(hopMac) == lattice::utils::MacAddress(deviceMacAddress))
    return nullptr;

  // Auto-register the chosen next hop as an unencrypted ESP-NOW peer (spec §3).
  // Idempotent — registerPeerWithEspNow no-ops if the peer already exists.
  lattice::mesh::crypto::registerPeerWithEspNow(hopMac);

  memcpy(nextHopScratch.mac, hopMac, 6);
  return &nextHopScratch;
}
```

Ensure `#include "MeshCrypto.h"` is already present in `Mesh.cpp` (it is — `registerPeerWithEspNow`/`generateKeypair` are used elsewhere; if not, add it).

- [ ] **Step 5: Run test + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: `NextHopThroughRelayIsRegisteredAsEspNowPeer` PASSES; **all existing tests still green.** Pay attention to the direct-master single-hop tests (`test_mesh_logic.cpp`, `test_route_report.cpp`) — the new `direct`/distance-1 branch preserves their behavior. If a test previously relied on `findNextHopToMaster` matching `currentMaster.nextHop` against an enrolled non-master peer at distance 1, it will now also be satisfiable via the NeighborTable branch; investigate any failure rather than weakening the test.

- [ ] **Step 6: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git add main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat: route uplink through NeighborTable next hop; auto-register relay peer"
```

---

### Task 5: Enable the multi-hop e2e test (close gap #7)

**Files:**
- Modify: `tests/e2e/scenarios/test_multihop_e2e.cpp` (drop `DISABLED_`, refresh comment)
- Modify: `docs/design-gaps/multihop-data-uplink.md` (status)

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces: `SensorOutOfMasterRangeRelaysThroughMiddleNode` enabled and passing — the executable spec for gap #7.

- [ ] **Step 1: Run the still-disabled test by name to confirm it now passes**

The test is currently `DISABLED_SensorOutOfMasterRangeRelaysThroughMiddleNode`. GoogleTest runs disabled tests only with the flag. Confirm the implementation makes it pass BEFORE dropping the prefix:

```bash
cmake --build tests/build --target lattice_e2e --parallel
./tests/build/lattice_e2e --gtest_also_run_disabled_tests --gtest_filter='*SensorOutOfMasterRangeRelaysThroughMiddleNode*'
```
Expected: PASS (leaf→relay→master uplink delivers to hub; `hop_count == 1`, `last_hop == relay`). If it fails, fix the implementation in the relevant earlier task — do NOT edit the test's assertions.

- [ ] **Step 2: Enable the test and refresh the stale comment**

In `tests/e2e/scenarios/test_multihop_e2e.cpp`:
- Rename `TEST_F(MeshSimTest, DISABLED_SensorOutOfMasterRangeRelaysThroughMiddleNode)` → `TEST_F(MeshSimTest, SensorOutOfMasterRangeRelaysThroughMiddleNode)`.
- Replace the stale block comment above it (the paragraph describing the `err::fail`/no-route gap and "Re-enable ... once the gap is closed") with a brief accurate note:

```cpp
// Multi-hop data uplink (gap #7, closed in Phase 2). A leaf out of direct RF
// range of the master enrolls through the relay (Phase 1 enrollment relay) and
// now uplinks sealed adapter data through it: the leaf learns the relay as a
// distance-1 neighbor from relayed beacons (NeighborTable, spec §3),
// findNextHopToMaster() selects it and auto-registers it as an unencrypted
// ESP-NOW peer, and the relay forwards the sealed frame it cannot read.
```

- [ ] **Step 3: Run the full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: the test now runs as part of the normal suite and passes; disabled count drops from 2 to 1 (only `RouteReportCarriesHopChain` remains disabled — that needs Phase 3 downlink source routing, out of scope).

- [ ] **Step 4: Update the design-gap doc**

In `docs/design-gaps/multihop-data-uplink.md`, change the `**Status:**` line to:

```markdown
**Status:** CLOSED (Phase 2, 2026-07-20) — multi-hop data uplink works via the
NeighborTable (spec §3). The dual-master data-failover sibling (#8) remains open,
tracked below and in the Phase 4 plan.
```

Leave the rest of the doc as historical record.

- [ ] **Step 5: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror tests/e2e/scenarios/test_multihop_e2e.cpp
git add tests/e2e/scenarios/test_multihop_e2e.cpp docs/design-gaps/multihop-data-uplink.md
git commit -m "test: enable multi-hop uplink e2e — closes design gap #7"
```

---

### Task 6: Finish — full verification + PR

**Files:** none (verification + PR only).

- [ ] **Step 1: Clean-from-scratch build + full suite**

```bash
rm -rf tests/build && cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: all pass from scratch; **1 disabled** (`RouteReportCarriesHopChain`, Phase 3).

- [ ] **Step 2: Whole-tree clang-format check (the CI gate)**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
find main/src \( -name '*.cpp' -o -name '*.h' \) ! -path '*/nanopb/*' ! -name 'mesh.pb.h' ! -name 'mesh.pb.c' | xargs "$CF" --style=file --dry-run --Werror
```
Expected: no output, exit 0.

- [ ] **Step 3: Push and open the PR**

Use the superpowers:finishing-a-development-branch skill. No submodule/protocol change this phase (v0.4.0 unchanged), so it's a single lattice-nodes PR. PR body must note: closes gap #7; NeighborTable is routing-only (no key material); relays forward sealed ciphertext; 1 remaining disabled test is Phase 3 scope.

---

## Deferred (explicitly NOT in Phase 2)

- **Downlink source routing** (`route_path` header, master route table) — Phase 3. `RouteReportCarriesHopChain` stays disabled.
- **Dual-master data failover** (gap #8, `secondary_*` keying) — Phase 4.
- **`route_path` header accumulation for route reports** — Phase 3 (relay branch still forwards route reports via the same NeighborTable next hop; that already works for uplink, but the master-side path table is Phase 3).
- **MAX_ROUTE_PATH_LEN staleness** (`project_config.h:120`, sized for the removed in-payload path encoding) — clean up in Phase 3 when the header `route_path` becomes authoritative.
- **`transmitCore` target rewrite on relayed sealed frames** (flagged Phase 1) — only matters when a relay's `currentMaster.mac` differs from the origin's target; in a single-master mesh it's a no-op. Revisit if/when multiple masters coexist (Phase 4).
