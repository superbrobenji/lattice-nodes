<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phase 3: Downlink Source Routing + k_down Sealing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The master delivers commands to a multi-hop node by source-routing a sealed downlink frame along the reversed path it learned from that node's route reports; relays forward statelessly; downlink payloads are E2E-sealed with `k_down`, closing the Phase-1 command-injection window.

**Architecture:** Route reports accumulate the relay chain in the plaintext `route_path[]` header field (relays append their own MAC in flight — legal because `route_path`/`route_len` are excluded from the AEAD AAD). The master records each node's path in a RAM `RouteTable` (node MAC → path). To send a downlink, the master reverses that path into `route_path[]`, sets `target_mac_address` to the destination, seals the payload with the destination's `k_down`, and unicasts to the first hop; each relay finds its own MAC in `route_path[]` and forwards to the next index (or to `target_mac` when it is the last relay). If no route is known, it falls back to the existing broadcast flood (still sealed). The destination node opens the payload with its `k_down`.

**Tech Stack:** C++ (ESP32 firmware, host-tested), GoogleTest + `tests/e2e` sim harness. `main/lib/lattice-protocol` submodule at v0.4.0 (proto v3, unchanged — `route_len`/`route_path[60]`/`auth_tag[16]` already exist).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-16-multihop-routing-e2e-crypto-design.md` §1 (wire format), §2 (keying, direction-split), §4 (downlink source routing).
- **`route_path` orientation (uplink accumulation):** a route report leaves the origin with `route_len = 0`. Each relay appends its own 6-byte MAC at `route_path[route_len*6 .. +6)` and increments `route_len`, BEFORE forwarding onward. The master therefore receives the relay chain in **origin-to-master forward order**: `[R_nearest_origin, …, R_nearest_master]`.
- **`route_path` orientation (downlink):** the master sends with `route_path[]` = the **reversed** learned path (`[R_nearest_master, …, R_nearest_origin]`) and `target_mac_address` = the destination node. A relay finds its own MAC at index `i` in `route_path[0..route_len)`; it forwards to `route_path[i+1]` if `i+1 < route_len`, else to `target_mac_address`. Relays hold **zero** routing state.
- **Bounds (parse-safety, every access):** `route_len <= MAX_HOPS` (10); reject/drop any frame with `route_len > MAX_HOPS`. Never index `route_path` beyond `route_len*6`; `route_path` is 60 bytes = 10 × 6, exactly `MAX_HOPS` MACs. Same defect class as the OOB ack-buffer fix (PR #31) — treat every `route_len`/index as attacker-controlled and bounds-check before use.
- **AAD exclusion:** `route_len`, `route_path`, `hop_count`, `last_hop_mac_address`, `auth_tag` are NOT in the AEAD AAD (verified: `E2ECrypto.h buildAad` binds only proto_version, message_type, data_type, origin, target, epoch, seq). Relays may rewrite `route_path`/`hop_count`/`last_hop` without breaking the tag. **`target_mac_address` IS AAD-bound** — the master sets it to the destination before sealing and no relay may change it.
- **Keying (spec §2):** downlink is sealed with `k_down`, derived per `(node, master)` pair. The master seals with the **destination node's** `k_down` (`peerE2EKeys(destMac)` → use `kDown`); the destination opens with its own `k_down` (`masterE2EKeys()` → use `kDown`). Both sides derive the same `k_down` from the same ECDH secret. Uplink continues to use `k_up` (unchanged).
- **Downlink nonce:** `epoch(4) || seq(2) || origin_mac(6)` with origin = the master's MAC and the master's own `nextSeqGuarded()`/`bootEpoch`. Unique per master message; reusing the same nonce across different destinations is safe because each destination uses a different `k_down` key.
- **What is sealed:** master-originated `MESH_TYPE_ADAPTER_DATA` addressed to a specific node (`target_mac_address != FF:FF:FF:FF:FF:FF`). Broadcast frames (beacons, enrollment, JOIN_ACK, any `FF:FF` adapter data) stay plaintext as today.
- **Fallback preserved:** if the master has no route-table entry for the destination, it still seals the payload but delivers via the existing broadcast flood (`broadcastToAllPeers`/`relayDownlink`) with `target_mac = destination` (not `FF:FF`). Only the destination opens it. This is the enrollment-time / unknown-route fallback (spec §4).
- **`LATTICE_ROUTE_TABLE_MAX`** (new, `lattice::config`, default **32**) bounds the master route table; **`LATTICE_NEIGHBOR_MAX`** = 8, **`MAX_HOPS`** = 10 (existing).
- PeerRegistry enrollment-only rule and the NeighborTable (Phase 2) are unchanged.
- All firmware test hooks `#ifdef UNIT_TEST`-gated.
- **clang-format 18** (CI `lint-format` checks `main/src` only — not `tests/`). Install once `python3 -m pip install 'clang-format==18.1.8'`; before each commit: `CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")` then `"$CF" --style=file --dry-run --Werror <changed main/src files>`. Only reformat lines you changed. The local `ctest` loop does NOT run clang-format.
- **CodeQL:** function params take `const uint8_t*`, never `const uint8_t mac[6]` (alert #300 class); bounds-check every buffer index (alert class from PR #31).
- Verification loop: `cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release` (once), then `cmake --build tests/build --parallel` and `ctest --test-dir tests/build --output-on-failure`. Baseline is **159 pass / 1 disabled** (`RouteReportCarriesHopChain`, which this phase enables). No device build in CI.

## Current-state facts (verified on this branch, post-Phase-2)

- `mesh_message` (v3): `route_len` (u8), `route_path[60]`, `auth_tag[16]` present; unused today. `buildMessage` (`Mesh.cpp:141-159`) does `mesh_message msg = {}` — zero-inits `route_len`/`route_path`.
- `sendRouteReport()` (`Mesh.cpp:913-921`): non-master, requires a route to master, sends `data[0]=OP_ROUTE_REPORT, data[1]=0`, `MESH_TYPE_ROUTE_REPORT`. Does NOT touch `route_path`.
- `processRouteReport()` (`Mesh.cpp:923-961`): master branch opens with `peerE2EKeys(origin).kUp`, checks `data[0]==OP_ROUTE_REPORT`, delivers to `externalRecvCallback`. Relay branch bumps `hop_count`/`last_hop`, forwards sealed `data` unmodified via `transmitCore(..., &relay)`. Neither branch touches `route_path`.
- `relayDownlink()` (`Mesh.cpp:758-769`): floods to enrolled `peers.peerMacs[]` (not NeighborTable), bumps hop_count/last_hop.
- `broadcastAdapterData()` (`Mesh.cpp:529-536`): `buildMessage`, `memset(target,0xFF,6)`, `broadcastToAllPeers`, optional local deliver. Static entry `broadcastAdapterDataStatic` is the master's serial→mesh downlink path (`SerialAdapter.cpp:279,356`).
- `processAdapterData()` (`Mesh.cpp:684-756`): computes `addressedToSelf`/`isBroadcastTarget`/`addressedToMaster` from `target_mac_address`. Master opens uplink (`k_up`) on self-addressed sealed frames (Phase 1); the Phase-1 CRITICAL guard drops sealed-type frames at the master not addressed to self. **Nodes do NOT currently open anything** (downlink is plaintext today).
- `Adapter::onMeshData` (`Adapter.cpp:41-72`): for `data_type==SERIAL_ADAPTER` && `data[0]==OP_CONFIG_SET`, reads `data[1..6]` as target MAC (or `FF:FF` broadcast), compares to own MAC, restarts with new adapter type. **Keep this**: the master seals the full 64-byte `data` (including the `data[1..6]` target bytes), the node opens then runs this existing check unchanged.
- `masterE2EKeys(const uint8_t** kUp, const uint8_t** kDown)` / `peerE2EKeys(const uint8_t* originMac, const uint8_t** kUp, const uint8_t** kDown)` (`Mesh.cpp:340-365`) — both return the pair; Phase 1 used only `kUp`. This phase uses `kDown`.
- `crypto::sealPayload(const uint8_t* key32, mesh_message&)` / `openPayload(...)` (`E2ECrypto.h`) — seal/open `data` in place using `auth_tag`.
- `currentMaster.nextHop` (`MasterInfo`, `PeerRegistry.h:21-25`): write-only/dead post-Phase-2 (writes at `Mesh.cpp:31,567,645`; no readers). `MAX_ROUTE_PATH_LEN` (`project_config.h:124`): stale, zero usages.
- Disabled test `RouteReportCarriesHopChain` (`tests/e2e/scenarios/test_route_report_e2e.cpp:60-83`): asserts the OLD in-payload encoding (`m.data[1]`=pathLen, `m.data[2..]`=MAC). Its own comment says to REWRITE for the header-field design before enabling. This phase rewrites its assertions to read `route_len`/`route_path` (the payload is sealed and opaque on the wire).

## File Structure

- **Create** `main/src/mesh/RouteTable.h` — master-side RAM route table (node MAC → path), header-only, non-copyable, bounded + evicting (mirrors `NeighborTable.h`/`E2EKeyStore.h`).
- **Create** `tests/unit/test_route_table.cpp` — RouteTable unit tests.
- **Modify** `main/project_config.h` — add `LATTICE_ROUTE_TABLE_MAX`; delete stale `MAX_ROUTE_PATH_LEN`.
- **Modify** `main/src/mesh/PeerRegistry.h` — remove dead `MasterInfo::nextHop`.
- **Modify** `main/src/mesh/Mesh.h` / `Mesh.cpp` — route_path accumulation in route-report relay; RouteTable member + recording; source-routed sealed downlink send + stateless relay forwarding; node-side `k_down` open; remove `currentMaster.nextHop` writes.
- **Modify** `main/src/adapter/serial/SerialAdapter.cpp` — master's CONFIG_SET/command downlink uses the new targeted-downlink API.
- **Modify** `tests/CMakeLists.txt` — register `test_route_table`.
- **Modify** `tests/e2e/scenarios/test_route_report_e2e.cpp` — rewrite + enable `RouteReportCarriesHopChain`.
- **Modify** `tests/unit/test_mesh_logic.cpp`, `tests/unit/test_route_report.cpp` — update fixtures that seed the removed `currentMaster.nextHop`.

---

### Task 1: Config + dead-field cleanup

**Files:**
- Modify: `main/project_config.h` (add `LATTICE_ROUTE_TABLE_MAX`, delete `MAX_ROUTE_PATH_LEN`)
- Modify: `main/src/mesh/PeerRegistry.h` (remove `MasterInfo::nextHop`)
- Modify: `main/src/mesh/Mesh.cpp` (remove the 2 `currentMaster.nextHop` writes)
- Modify: `tests/unit/test_mesh_logic.cpp`, `tests/unit/test_route_report.cpp` (remove `.nextHop` seed writes)

**Interfaces:**
- Produces: `lattice::config::LATTICE_ROUTE_TABLE_MAX` (`size_t`, 32). `MasterInfo` becomes `{ uint8_t mac[6]; uint8_t distance; }`.

- [ ] **Step 1: Add the knob, delete the stale one**

In `main/project_config.h`, after the `LATTICE_NEIGHBOR_MAX` line add:

```cpp
// Downlink source routing (spec §4): max node->path entries the master tracks.
// Master is hub-side with RAM headroom; raise for large deployments.
inline constexpr size_t LATTICE_ROUTE_TABLE_MAX = 32;
```

Delete the `MAX_ROUTE_PATH_LEN` line (stale, sized for the removed in-payload encoding; grep confirms zero usages — `grep -rn MAX_ROUTE_PATH_LEN main/ tests/` must return nothing after deletion).

- [ ] **Step 2: Remove the dead `nextHop` field**

In `main/src/mesh/PeerRegistry.h`, change `MasterInfo` to drop `nextHop`:

```cpp
struct MasterInfo {
  uint8_t mac[6];
  uint8_t distance; // Hops to master
};
```

- [ ] **Step 3: Remove writers + test seeds**

`grep -rn "\.nextHop\|currentMaster.nextHop\|MasterInfo" main/ tests/` and remove every write to `currentMaster.nextHop`:
- `Mesh.cpp:31` (constructor zero-init line for nextHop), `Mesh.cpp:567` (checkMasterTimeout), `Mesh.cpp:645` (processMasterBeacon `memcpy(currentMaster.nextHop, ...)`). Delete those statements (the surrounding `mac`/`distance` handling stays).
- `tests/unit/test_mesh_logic.cpp:339,674` and `tests/unit/test_route_report.cpp:38` — delete the lines that write `.nextHop` on a `currentMaster`/`MasterInfo` (the tests never read it; they only need `mac`/`distance`).

- [ ] **Step 4: Build + full suite**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: 159 pass / 1 disabled (no behavior change — dead field removed, new constant unused so far).

- [ ] **Step 5: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/project_config.h main/src/mesh/PeerRegistry.h main/src/mesh/Mesh.cpp
git add main/project_config.h main/src/mesh/PeerRegistry.h main/src/mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp tests/unit/test_route_report.cpp
git commit -m "refactor: add LATTICE_ROUTE_TABLE_MAX; remove dead MasterInfo::nextHop and stale MAX_ROUTE_PATH_LEN"
```

---

### Task 2: RouteTable (master-side, unit-tested in isolation)

**Files:**
- Create: `main/src/mesh/RouteTable.h`
- Create: `tests/unit/test_route_table.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `lattice::config::LATTICE_ROUTE_TABLE_MAX`, `MAX_HOPS`, `STALE` — none needed at runtime beyond the max; staleness uses a caller-passed `nowMillis`.
- Produces: `class lattice::mesh::RouteTable` with:
  - `void record(const uint8_t* nodeMac, const uint8_t* path, uint8_t pathLen, uint32_t nowMillis)` — insert/update the node's path (`pathLen <= MAX_HOPS`; a `pathLen > MAX_HOPS` call is ignored). On a full table with no existing entry, evict the oldest (smallest `lastSeenMillis`).
  - `bool lookup(const uint8_t* nodeMac, uint8_t* pathOut, uint8_t* pathLenOut) const` — copy the node's stored path into `pathOut` (caller supplies a `>= 60`-byte buffer) and length into `*pathLenOut`; return `true` if found, else `false`.
  - `void clear()`; non-copyable.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_route_table.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/RouteTable.h"

using namespace lattice::mesh;

static const uint8_t NODE[6] = {0x02, 0, 0, 0, 0, 0xEE};
static const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
static const uint8_t R2[6] = {0x02, 0, 0, 0, 0, 0x22};

TEST(RouteTable, RecordsAndLooksUpPath) {
  RouteTable t;
  uint8_t path[12];
  memcpy(path, R1, 6);
  memcpy(path + 6, R2, 6);
  t.record(NODE, path, 2, 1000);
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(NODE, out, &outLen));
  EXPECT_EQ(outLen, 2);
  EXPECT_EQ(0, memcmp(out, R1, 6));
  EXPECT_EQ(0, memcmp(out + 6, R2, 6));
}

TEST(RouteTable, LookupMissReturnsFalse) {
  RouteTable t;
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}

TEST(RouteTable, RecordUpdatesExistingNode) {
  RouteTable t;
  uint8_t p1[6];
  memcpy(p1, R1, 6);
  t.record(NODE, p1, 1, 1000); // path via R1
  uint8_t p2[12];
  memcpy(p2, R2, 6);
  memcpy(p2 + 6, R1, 6);
  t.record(NODE, p2, 2, 2000); // newer path via R2,R1
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(NODE, out, &outLen));
  EXPECT_EQ(outLen, 2);
  EXPECT_EQ(0, memcmp(out, R2, 6));
}

TEST(RouteTable, RejectsOverlongPath) {
  RouteTable t;
  uint8_t big[66] = {};
  t.record(NODE, big, 11, 1000); // 11 > MAX_HOPS(10) → ignored
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}

TEST(RouteTable, EvictsOldestWhenFull) {
  RouteTable t;
  uint8_t p[6] = {0x02, 0, 0, 0, 0, 0x01};
  // Fill: node i observed at time (i+1)*100; node 0 is oldest.
  for (size_t i = 0; i < lattice::config::LATTICE_ROUTE_TABLE_MAX; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(0x80 + i)};
    t.record(mac, p, 1, static_cast<uint32_t>((i + 1) * 100));
  }
  uint8_t oldest[6] = {0x02, 0, 0, 0, 0, 0x80}; // observed at t=100
  uint8_t out[60], outLen = 0;
  ASSERT_TRUE(t.lookup(oldest, out, &outLen));
  // Insert one more (fresh) node → oldest must be evicted.
  t.record(NODE, p, 1, 999999);
  EXPECT_FALSE(t.lookup(oldest, out, &outLen)) << "oldest entry evicted";
  EXPECT_TRUE(t.lookup(NODE, out, &outLen)) << "new entry present";
}

TEST(RouteTable, ClearEmpties) {
  RouteTable t;
  uint8_t p[6] = {0x02, 0, 0, 0, 0, 0x01};
  t.record(NODE, p, 1, 1000);
  t.clear();
  uint8_t out[60], outLen = 0;
  EXPECT_FALSE(t.lookup(NODE, out, &outLen));
}
```

Add to `tests/CMakeLists.txt` next to the other `add_unit_test` lines (mirror `test_neighbor_table`):

```cmake
add_unit_test(test_route_table unit/test_route_table.cpp)
```

- [ ] **Step 2: Run to verify RED**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --target test_route_table --parallel
```
Expected: FAIL — `RouteTable.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `main/src/mesh/RouteTable.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "../../project_config.h"

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

  bool lookup(const uint8_t* nodeMac, uint8_t* pathOut, uint8_t* pathLenOut) const {
    for (size_t i = 0; i < config::LATTICE_ROUTE_TABLE_MAX; ++i) {
      const Entry& e = entries[i];
      if (e.valid && memcmp(e.nodeMac, nodeMac, 6) == 0) {
        *pathLenOut = e.pathLen;
        if (e.pathLen)
          memcpy(pathOut, e.path, static_cast<size_t>(e.pathLen) * 6);
        return true;
      }
    }
    return false;
  }

  void clear() {
    memset(entries, 0, sizeof(entries));
  }

private:
  struct Entry {
    uint8_t nodeMac[6];
    uint8_t pathLen;
    bool valid;
    uint32_t lastSeenMillis;
    uint8_t path[config::MAX_HOPS * 6]; // 60 bytes, matches route_path[]
  };
  Entry entries[config::LATTICE_ROUTE_TABLE_MAX]{};

  Entry* findSlot(const uint8_t* nodeMac) {
    for (size_t i = 0; i < config::LATTICE_ROUTE_TABLE_MAX; ++i)
      if (entries[i].valid && memcmp(entries[i].nodeMac, nodeMac, 6) == 0)
        return &entries[i];
    return nullptr;
  }

  Entry* allocateSlot() {
    for (size_t i = 0; i < config::LATTICE_ROUTE_TABLE_MAX; ++i)
      if (!entries[i].valid)
        return &entries[i];
    Entry* oldest = &entries[0];
    for (size_t i = 1; i < config::LATTICE_ROUTE_TABLE_MAX; ++i)
      if (entries[i].lastSeenMillis < oldest->lastSeenMillis)
        oldest = &entries[i];
    return oldest;
  }
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 4: Run to verify GREEN + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: 6 new tests pass; full suite 165 pass / 1 disabled.

- [ ] **Step 5: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/RouteTable.h tests/unit/test_route_table.cpp
git add main/src/mesh/RouteTable.h tests/unit/test_route_table.cpp tests/CMakeLists.txt
git commit -m "feat: RouteTable — master-side node->path store for downlink source routing"
```

---

### Task 3: Route reports accumulate the relay path in the header

**Files:**
- Modify: `main/src/mesh/Mesh.cpp` (`processRouteReport` relay branch)
- Modify: `tests/unit/test_route_report.cpp` (assert path accumulation)

**Interfaces:**
- Consumes: `route_len`/`route_path` header fields; `MAX_HOPS`.
- Produces: a route report arriving at the master carries `route_path[0..route_len)` = the relay chain in origin-to-master order.

- [ ] **Step 1: Write the failing unit test**

Add to `tests/unit/test_route_report.cpp` (mirror the existing relay-forward test's fixture — read how it builds a `mesh_message` route report and invokes the relay path):

```cpp
TEST_F(RouteReportRelayTest, RelayAppendsOwnMacToRoutePath) {
  Mesh relay = makeIntermediateNode(); // non-master, has a route to master (fixture)
  mesh_message rr = {};
  rr.proto_version = PROTO_VERSION;
  rr.message_type = MESH_TYPE_ROUTE_REPORT;
  const uint8_t leafMac[6] = {0x02, 0, 0, 0, 0, 0x0B};
  memcpy(rr.origin_mac_address, leafMac, 6);
  memcpy(rr.target_mac_address, relay.currentMaster.mac, 6);
  rr.epoch_num = 5;
  rr.seq_num = 1;
  rr.hop_count = 0;
  rr.route_len = 0; // origin sent an empty path

  mesh_message forwarded;
  relay.testCaptureNextSend(&forwarded); // fixture hook capturing the outgoing frame
  relay.testProcessRouteReport(rr);       // UNIT_TEST hook onto processRouteReport

  ASSERT_EQ(forwarded.route_len, 1) << "relay appended its own MAC";
  EXPECT_EQ(0, memcmp(&forwarded.route_path[0], relay.testDeviceMac(), 6));
}
```

Use whatever capture/dispatch hooks the fixture already exposes; if `processRouteReport` isn't directly reachable, drive it through the recv queue the way other relay tests do, and read the forwarded frame from the esp_now mock's last-sent record. Add minimal `#ifdef UNIT_TEST` hooks to `Mesh.h` only if no existing path reaches it (e.g. `void testProcessRouteReport(const mesh_message& m){ processRouteReport(m); }`, `const uint8_t* testDeviceMac(){ return deviceMacAddress; }`). Prefer the existing esp_now mock's sent-frame query over adding a bespoke capture hook.

- [ ] **Step 2: Run to verify RED**

```bash
cmake --build tests/build --target test_route_report --parallel && ctest --test-dir tests/build -R RelayAppendsOwnMacToRoutePath --output-on-failure
```
Expected: FAIL — `route_len` stays 0 (relay doesn't append yet).

- [ ] **Step 3: Implement path accumulation**

In `main/src/mesh/Mesh.cpp` `processRouteReport` relay branch (currently bumps `hop_count`/`last_hop` then forwards), append this node's MAC to `route_path` before forwarding. Replace the relay branch body with:

```cpp
  // Relay node (spec §4): the payload is E2E-sealed origin->master and opaque to
  // us. Accumulate the relay path in the plaintext route_path header (excluded
  // from AAD, so this does not break the tag) so the master learns the full
  // origin->master relay chain for downlink source routing.
  if (msg.hop_count >= lattice::config::MAX_HOPS)
    return;
  if (msg.route_len >= lattice::config::MAX_HOPS) {
    Logger::logln("MESH", "route report path full — dropping", LogLevel::LOG_WARN);
    return;
  }
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  memcpy(&relay.route_path[static_cast<size_t>(relay.route_len) * 6], deviceMacAddress, 6);
  relay.route_len++;
  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ROUTE_REPORT,
               &relay);
```

- [ ] **Step 4: Run to verify GREEN + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: new test passes; full suite still green (165 pass / 1 disabled — the disabled e2e test is enabled in Task 6).

- [ ] **Step 5: clang-format + commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Mesh.cpp main/src/mesh/Mesh.h
git add main/src/mesh/Mesh.cpp main/src/mesh/Mesh.h tests/unit/test_route_report.cpp
git commit -m "feat: route reports accumulate relay path in route_path header (spec §4)"
```

---

### Task 4: Master records routes from reports

**Files:**
- Modify: `main/src/mesh/Mesh.h` (add `RouteTable routes;` member + `#ifdef UNIT_TEST` accessor), `Mesh.cpp` (`processRouteReport` master branch)

**Interfaces:**
- Consumes: `RouteTable::record` (Task 2); the opened route report.
- Produces: `Mesh::routes` populated with `origin_mac → route_path` on every valid route report the master receives. Adds `#ifdef UNIT_TEST RouteTable& testRoutes()`.

- [ ] **Step 1: Add member + include + hook**

In `Mesh.h`: `#include "RouteTable.h"`; add private `RouteTable routes;` near `NeighborTable neighbors;`; add under the `#ifdef UNIT_TEST` block: `RouteTable& testRoutes() { return routes; }`.

- [ ] **Step 2: Write the failing test**

Add to `tests/unit/test_route_report.cpp` a master-side test: build a master `Mesh`, feed a route report (origin = a leaf, `route_len=2`, `route_path=[R1,R2]`) sealed with that leaf's `k_up` (reuse the fixture's key setup used by the existing master-open test), drive `processRouteReport`, then assert `master.testRoutes().lookup(leafMac, out, &len)` returns `len==2`, `out[0..6]==R1`. Mirror the existing master-branch route-report unit test for the sealing/keys setup.

- [ ] **Step 3: Run RED**, then **Step 4: implement**:

In `processRouteReport` master branch, after the payload opens and `opened.data[0]==OP_ROUTE_REPORT` is verified, record the route (use the RAW `msg`'s header path — `route_path` is plaintext, not part of the sealed payload):

```cpp
    // Learn the origin's relay path for downlink source routing (spec §4).
    // route_path/route_len are plaintext header fields (accumulated by relays);
    // bounds-checked by RouteTable::record.
    routes.record(msg.origin_mac_address, msg.route_path, msg.route_len, millis());
```

- [ ] **Step 5: GREEN + full suite; clang-format; commit** `"feat: master records node routes from route reports"`.

Expected suite after: 166 pass / 1 disabled.

---

### Task 5: Source-routed sealed downlink send (master) + stateless relay forwarding

**Files:**
- Modify: `main/src/mesh/Mesh.h` / `Mesh.cpp` (new `sendDownlinkToNode`; source-route forwarding in `processAdapterData`)
- Modify: `tests/unit/test_mesh_logic.cpp` (unit tests for send + forward)

**Interfaces:**
- Consumes: `RouteTable::lookup`, `peerE2EKeys(destMac).kDown`, `sealPayload`, `findNextHopToMaster` (not used here), `sendMessage`, `broadcastToAllPeers`.
- Produces:
  - `void Mesh::sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data)` — master-only. Builds a `MESH_TYPE_ADAPTER_DATA` frame, `target_mac_address = destMac`, seals `data` with the destination's `k_down`, then: if `routes.lookup(destMac)` succeeds → reverse the path into `route_path[]`, set `route_len`, unicast to `route_path[0]` (or directly to `destMac` if `route_len==0`); else → `route_len=0` and broadcast-flood fallback (`broadcastToAllPeers`). Returns void; logs+drops if keys unavailable.
  - Stateless downlink relay: in `processAdapterData`, a non-master node that is not the target and sees `route_len > 0` finds its own MAC in `route_path[0..route_len)` and forwards to the next hop; if not found or `route_len==0`, falls back to the existing `relayDownlink` flood.

- [ ] **Step 1: Write failing unit tests**

Add to `tests/unit/test_mesh_logic.cpp`:

```cpp
// (a) Master with a known route source-routes to the first hop, target=dest, sealed.
TEST_F(JoinAckRelayTest, DownlinkSourceRoutesViaFirstHop) {
  Mesh master = makeMasterNode(); // fixture master with an enrolled leaf + keys
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
  const uint8_t R2[6] = {0x02, 0, 0, 0, 0, 0x22};
  uint8_t path[12];
  memcpy(path, R1, 6);
  memcpy(path + 6, R2, 6); // origin->R1->R2->master
  master.testRoutes().record(leaf, path, 2, 1000);

  resetEspNowMock();
  uint8_t cmd[64] = {};
  cmd[0] = OP_CONFIG_SET;
  master.sendDownlinkToNode(leaf, adapter_types::SERIAL_ADAPTER, cmd);

  // Reversed path [R2,R1]; first hop = R2.
  mesh_message sent = lastEspNowSentTo(R2); // fixture/mock query
  ASSERT_TRUE(wasSentTo(R2));
  EXPECT_EQ(sent.route_len, 2);
  EXPECT_EQ(0, memcmp(&sent.route_path[0], R2, 6));
  EXPECT_EQ(0, memcmp(&sent.route_path[6], R1, 6));
  EXPECT_EQ(0, memcmp(sent.target_mac_address, leaf, 6));
  // payload sealed: data[0] != plaintext opcode
  EXPECT_NE(sent.data[0], OP_CONFIG_SET);
  // first hop auto-registered as ESP-NOW peer (VirtualBus doesn't enforce this,
  // so assert it explicitly — the Phase-2 lesson).
  EXPECT_TRUE(esp_now_is_peer_exist(R2));
}

// (b) A relay in the path forwards to the next index, unchanged frame.
TEST_F(JoinAckRelayTest, DownlinkRelayForwardsToNextIndex) {
  Mesh r2 = makeIntermediateNodeWithMac(/*R2*/ {0x02,0,0,0,0,0x22});
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  const uint8_t R1[6] = {0x02, 0, 0, 0, 0, 0x11};
  mesh_message dl = {};
  dl.proto_version = PROTO_VERSION;
  dl.message_type = MESH_TYPE_ADAPTER_DATA;
  memcpy(dl.target_mac_address, leaf, 6);
  dl.route_len = 2;
  memcpy(&dl.route_path[0], /*R2*/ r2.testDeviceMac(), 6);
  memcpy(&dl.route_path[6], R1, 6);
  dl.epoch_num = 7; dl.seq_num = 3;

  resetEspNowMock();
  r2.testProcessAdapterData(dl); // UNIT_TEST hook
  EXPECT_TRUE(wasSentTo(R1)) << "R2 forwards to route_path[1]=R1";
}

// (c) Last relay forwards to target_mac.
TEST_F(JoinAckRelayTest, DownlinkLastRelayForwardsToTarget) {
  const uint8_t leaf[6] = {0x02, 0, 0, 0, 0, 0x0B};
  Mesh r1 = makeIntermediateNodeWithMac({0x02,0,0,0,0,0x11}); // R1
  mesh_message dl = {};
  dl.proto_version = PROTO_VERSION;
  dl.message_type = MESH_TYPE_ADAPTER_DATA;
  memcpy(dl.target_mac_address, leaf, 6);
  dl.route_len = 2;
  const uint8_t R2mac[6] = {0x02, 0, 0, 0, 0, 0x22};
  memcpy(&dl.route_path[0], R2mac, 6);
  memcpy(&dl.route_path[6], r1.testDeviceMac(), 6); // R1 at index 1 (last)
  dl.epoch_num = 7; dl.seq_num = 4;
  resetEspNowMock();
  r1.testProcessAdapterData(dl);
  EXPECT_TRUE(wasSentTo(leaf)) << "last relay forwards to target_mac";
}
```

Adapt the fixture helpers (`makeMasterNode`, `makeIntermediateNodeWithMac`, `lastEspNowSentTo`/`wasSentTo`, `resetEspNowMock`) to what the test file + esp_now mock actually provide — read them first; add minimal `#ifdef UNIT_TEST` `testProcessAdapterData(const mesh_message&)` hook if `processAdapterData` isn't reachable.

- [ ] **Step 2: RED.** **Step 3: implement.**

Add to `Mesh.h` (public): `void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data);` and, if needed, the UNIT_TEST hook.

In `Mesh.cpp`:

```cpp
void Mesh::sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data) {
  if (!isMaster)
    return;
  mesh_message msg = buildMessage(type, data, MESH_TYPE_ADAPTER_DATA);
  memcpy(msg.target_mac_address, destMac, 6); // AAD-bound destination

  const uint8_t *kUp, *kDown;
  if (!peerE2EKeys(destMac, &kUp, &kDown) || !lattice::mesh::crypto::sealPayload(kDown, msg)) {
    Logger::logln("MESH", "downlink seal unavailable — dropped", LogLevel::LOG_WARN);
    return;
  }

  uint8_t path[lattice::config::MAX_HOPS * 6];
  uint8_t pathLen = 0;
  if (routes.lookup(destMac, path, &pathLen) && pathLen > 0) {
    // Reverse the origin->master path into master->origin order.
    msg.route_len = pathLen;
    for (uint8_t i = 0; i < pathLen; ++i)
      memcpy(&msg.route_path[static_cast<size_t>(i) * 6], &path[static_cast<size_t>(pathLen - 1 - i) * 6], 6);
    // First hop = route_path[0]; auto-register it as an unencrypted ESP-NOW peer
    // (idempotent) so esp_now_send can unicast to it. Command-frequency, so no
    // dedicated bounding beyond the enrolled/forwarding peers already present.
    lattice::mesh::crypto::registerPeerWithEspNow(msg.route_path);
    sendMessage(msg.route_path, msg);
    return;
  }
  // No known multi-hop route: fall back to broadcast flood (still sealed).
  // Direct/adjacent nodes and unknown-route nodes are reached this way.
  msg.route_len = 0;
  broadcastToAllPeers(msg);
}
```

In `processAdapterData`, the non-master downlink branch (`!isMaster && !addressedToSelf && !isBroadcastTarget && !addressedToMaster`) currently calls `relayDownlink(msg)`. Replace with source-route-then-fallback:

```cpp
    // Downlink toward a specific node. If the frame carries a source route and
    // we are on it, forward to the next hop (stateless — spec §4); otherwise
    // fall back to the flood.
    if (msg.route_len > 0 && msg.route_len <= lattice::config::MAX_HOPS) {
      for (uint8_t i = 0; i < msg.route_len; ++i) {
        if (memcmp(&msg.route_path[static_cast<size_t>(i) * 6], deviceMacAddress, 6) == 0) {
          if (msg.hop_count >= lattice::config::MAX_HOPS)
            return;
          mesh_message fwd = msg;
          fwd.hop_count++;
          const uint8_t* next = (i + 1 < msg.route_len)
                                    ? &msg.route_path[static_cast<size_t>(i + 1) * 6]
                                    : msg.target_mac_address;
          lattice::mesh::crypto::registerPeerWithEspNow(next); // idempotent; needed to unicast
          sendMessage(next, fwd);
          return;
        }
      }
    }
    relayDownlink(msg); // not on the route / no route → existing flood fallback
    return;
```

- [ ] **Step 4: GREEN + full suite; clang-format; commit** `"feat: source-routed sealed downlink send + stateless relay forwarding (spec §4)"`.

Expected suite after: 169 pass / 1 disabled.

---

### Task 6: Node opens k_down downlink; wire master command path; enable e2e

**Files:**
- Modify: `main/src/mesh/Mesh.cpp` (`processAdapterData` local-delivery open with `k_down`)
- Modify: `main/src/adapter/serial/SerialAdapter.cpp` (CONFIG_SET/command → `sendDownlinkToNode`)
- Modify: `tests/e2e/scenarios/test_route_report_e2e.cpp` (rewrite + enable `RouteReportCarriesHopChain`)
- Create/Modify e2e: a multi-hop sealed-downlink delivery scenario.

**Interfaces:**
- Consumes: `masterE2EKeys().kDown` (node side), `openPayload`, `sendDownlinkToNode` (Task 5).
- Produces: a distance-2 node receives, opens, and applies a master command; the gap-#7-sibling downlink path works end to end.

- [ ] **Step 1: Node-side open (failing e2e first)**

Write a multi-hop downlink e2e test in `tests/e2e/scenarios/` (mirror `test_multihop_e2e.cpp` chain fixture): master + relay + leaf (leaf NOT linked to master), enroll both, let route reports populate the master's table (`runPolled(ROUTE_REPORT_INTERVAL_MS + 5000)`), then have the master send a CONFIG_SET to the leaf via the serial path; assert the leaf receives and applies it (e.g. observes the adapter-type-change / restart signal the harness exposes, or the leaf's `externalRecvCallback` sees the opened CONFIG_SET with `data[0]==OP_CONFIG_SET`). Assert a relay in between never delivered it locally (only forwarded).

- [ ] **Step 2: RED**, then **Step 3: implement node-side open**

In `processAdapterData` local-delivery section (the `addressedToSelf` path on a non-master node), before the existing `externalRecvCallback`/config handling, open a sealed downlink:

```cpp
  // Node-side E2E open (spec §2): a self-addressed sealed ADAPTER_DATA from the
  // master is opened with our k_down before local delivery. Mirrors the master's
  // uplink open. Failure → drop (finding-#9 pattern).
  mesh_message opened = msg;
  if (!isMaster && addressedToSelf && msg.message_type == MESH_TYPE_ADAPTER_DATA) {
    const uint8_t *kUp, *kDown;
    if (!masterE2EKeys(&kUp, &kDown) || !lattice::mesh::crypto::openPayload(kDown, opened)) {
      Logger::logln("MESH", "downlink open failed — dropped", LogLevel::LOG_WARN);
      return;
    }
  }
```

and use `opened` for the subsequent local-delivery/config checks and `externalRecvCallback` on this node path (the existing master-side and broadcast handling keeps using its own opened/raw copy as today).

**Care:** confirm the existing single-hop `ServerBroadcastTest.ConfigSetReachesNodeAdapterAndPersists` path — the master must now seal single-hop targeted downlinks too, so the direct node opens them. Update the master command origination (Step 4) so BOTH single-hop and multi-hop go through `sendDownlinkToNode` (sealed). Broadcast-to-all (`FF:FF`) adapter data stays plaintext and is NOT opened (guarded by `addressedToSelf`, which is false for `FF:FF`).

- [ ] **Step 4: Wire the master command path**

In `main/src/adapter/serial/SerialAdapter.cpp` where the master handles a server CONFIG_SET / targeted command and currently calls `broadcastAdapterDataStatic` (lines ~279, ~356), extract the destination MAC (from the command's `data[1..6]`) and call the new targeted API instead:

```cpp
// Master → node command: source-route + seal to the specific destination.
uint8_t destMac[6];
memcpy(destMac, &fwdData[1], 6); // CONFIG_SET target field
lattice::mesh::Mesh::sendDownlinkToNodeStatic(destMac, static_cast<adapter_types>(msg.data_type), fwdData);
```

Add a static forwarder `Mesh::sendDownlinkToNodeStatic` mirroring `broadcastAdapterDataStatic` (it exists as a static→instance shim; follow that pattern). If the command is a genuine broadcast (target `FF:FF`), keep the existing `broadcastAdapterDataStatic` (plaintext broadcast) — only targeted commands get sealed source routing. Read the surrounding code to place this correctly and preserve HEALTH_REQ / other opcodes.

- [ ] **Step 5: Rewrite + enable `RouteReportCarriesHopChain`**

In `tests/e2e/scenarios/test_route_report_e2e.cpp`, rewrite the disabled test's assertions for the header-field design and drop `DISABLED_`. The payload is sealed/opaque on the wire, so assert on the header `route_len`/`route_path`, not `data[]`:

```cpp
TEST_F(MeshSimTest, RouteReportCarriesHopChain) {
  addMaster();
  auto* relay = addSensor(MAC_NODE_A);
  auto* leaf = addSensor(MAC_NODE_B);
  world.bus.link(master, relay);
  world.bus.link(relay, leaf);
  enroll(relay);
  enroll(leaf);
  runPolled(lattice::config::ROUTE_REPORT_INTERVAL_MS + 5000);

  bool sawLeafRoute = false;
  for (const auto& m : hub->ofType(MESH_TYPE_ROUTE_REPORT)) {
    if (memcmp(m.origin_mac_address, leaf->mac(), 6) != 0)
      continue;
    ASSERT_GE(m.route_len, 1);
    EXPECT_EQ(memcmp(&m.route_path[0], relay->mac(), 6), 0)
        << "first relay in the leaf's path is the middle node";
    sawLeafRoute = true;
  }
  EXPECT_TRUE(sawLeafRoute) << "leaf's route report reaches the hub carrying its relay path";
}
```

(If `FakeHub::ofType` surfaces the frame as received at the master, `route_path`/`route_len` are the plaintext header as the master saw it. Confirm the hub records the header fields; if it only records opened payload, read the frame via the master's recorded route table instead — `hub` exposes what the master delivered.)

- [ ] **Step 6: Full suite; clang-format; commit**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
"$CF" --style=file --dry-run --Werror main/src/mesh/Mesh.cpp main/src/adapter/serial/SerialAdapter.cpp
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
git add -A
git commit -m "feat: k_down downlink open + master command source routing; enable RouteReportCarriesHopChain (closes downlink gap + command-injection window)"
```
Expected: **0 disabled tests** remaining; full suite green (~171 pass / 0 disabled).

---

### Task 7: Finish — full verification + PR

- [ ] **Step 1: Clean-from-scratch build + suite**

```bash
rm -rf tests/build && cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```
Expected: all pass, **0 disabled**.

- [ ] **Step 2: Whole-tree clang-format-18 gate (main/src)**

```bash
CF=$(python3 -c "import clang_format,os;print(os.path.join(os.path.dirname(clang_format.__file__),'data','bin','clang-format'))")
find main/src \( -name '*.cpp' -o -name '*.h' \) ! -path '*/nanopb/*' ! -name 'mesh.pb.h' ! -name 'mesh.pb.c' | xargs "$CF" --style=file --dry-run --Werror
```
Expected: exit 0, no output.

- [ ] **Step 3: PR via superpowers:finishing-a-development-branch**

Single lattice-nodes PR (no protocol change; submodule stays v0.4.0). PR body: completes spec §4 downlink source routing + §2 downlink `k_down` sealing; closes the Phase-1 command-injection window; enables the last disabled test; `currentMaster.nextHop`/`MAX_ROUTE_PATH_LEN` cleanup. Note the gh active account must be `superbrobenji` (ADMIN) to open the PR.

---

## Deferred (explicitly NOT in Phase 3)

- **Dual-master data failover** (gap #8, `secondary_master_mac`/`secondary_public_key` fields already in the v3 struct) — Phase 4. This is the remaining open gap.
- **`transmitCore` AAD-bound target rewrite on relayed sealed frames** — only bites with multiple masters; Phase 4.
- **Route-report path authenticity:** the relay-accumulated `route_path` is plaintext and relay-asserted (a malicious relay can falsify it → misrouted downlink, DoS-class, same envelope as the spec §2/§4 accepted blackhole risk; uplink data integrity unaffected). Documented, not mitigated.
- **Sticky-low `currentMaster.distance`** (Phase-2 MINOR-2) — longer-only path repair latency; not addressed here.
