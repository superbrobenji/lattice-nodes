# Phase B — Mesh Subsystem Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Decompose `Mesh` (1382 lines, doing 6+ jobs beyond orchestration) into a thin orchestrator plus 3 focused collaborator classes, and fix 3 encapsulation breaks in `PeerRegistry`/`Enrollment`/`ReplayCache` — with zero behavior change anywhere.

**Architecture:** 7 tasks. Tasks 1-3 (encapsulation) are independent of each other and land first. Tasks 4-6 (collaborator extraction: `MeshTransport`, `MasterBeacon`, `DownlinkRouter`) depend on Tasks 1-3 landing (their new code calls the new private-field methods instead of reaching into raw fields) but are independent of each other. Task 7 (final wiring) depends on Tasks 4-6.

**Tech Stack:** C++17, ESP-IDF (host-mocked for tests), GoogleTest, CMake. `UNIT_TEST` is defined for all test targets — it makes `Mesh`'s members conditionally public for white-box test access (see `Mesh.h`'s class-level comment); this plan preserves that idiom throughout.

## Global Constraints

- **No wire-format changes, no behavior changes anywhere.** This is a pure structural refactor — every check, log line, and timing behavior stays byte-identical; only which class owns the code changes.
- **Firmware-only.** No `lattice-hub`/`lattice-protocol` touches.
- **Full unit + e2e regression required after every task.** Run `cmake --build tests/build --parallel 2 && ctest --test-dir tests/build --output-on-failure --label-exclude e2e` for unit tests, and drop `--label-exclude e2e` for the e2e suite, after every task's changes. Cap build parallelism at 2 (this machine OOMs on full-parallel builds).
- **`processAdapterData`'s security sequence (gate → E2E open → config-opcode authorization → deliver) does not move and does not get restructured beyond the one `switch` on `DownlinkRouter::classify()`'s result in Task 6.** It is one atomic, heavily-commented security check sequence — do not split it further, do not reorder its checks.
- **New collaborator classes stay crypto-free** (`MasterBeacon` and `DownlinkRouter` call back into `Mesh`-owned crypto/E2E operations via `Mesh` itself, never by holding their own copy of key material or a back-pointer to `Mesh`) — they take whatever external state they need as parameters, matching the existing `NeighborTable`/`RouteTable`/`E2EKeyStore` pattern in this codebase.
- **CI size delta reported per PR.** Expect near-zero — this refactor adds no virtual dispatch and no new heap allocation, only regroups existing code.

## Sequencing

```
Task 1 (PeerRegistry)  ─┐
Task 2 (Enrollment)    ─┼── independent of each other; land first so Tasks 4-6
Task 3 (ReplayCache)   ─┘   write each new call site once, not twice
Task 4 (MeshTransport) ── needs Task 1 (PeerRegistry's new iteration API in setupEspNow)
Task 5 (MasterBeacon)  ── needs Task 2 (learnMasterMac/learnSecondaryMasterMac) + Task 3 (txState)
                           + Task 4 (Task 5 Step 4 dispatches through the handleReceivedMessage/
                           MessageHandler skeleton Task 4 creates — Task 5 cannot start until
                           Task 4 is merged, not just Tasks 2-3)
Task 6 (DownlinkRouter)── needs Task 4 (MeshTransport::sendMessage, referenced by classify()'s caller)
Task 7 (Mesh thin orchestrator) ── needs Tasks 4-6 done
```

---

### Task 1: `PeerRegistry` encapsulation

**Files:**
- Modify: `firmware/main/src/mesh/PeerRegistry.h`
- Modify: `firmware/main/src/mesh/Mesh.h:402-403` (`getPeerList`/`getPeerCount`)
- Modify: `firmware/main/src/mesh/Mesh.cpp` — 5 sites: `setupEspNow:337-339`, `broadcastToAllPeers:478,482-485`, `relayDownlink:1048-1051`, `addPeer:1111,1113-1114`, `registerPeerWithKey:1140`
- Modify: `tests/e2e/scenarios/test_harness_smoke.cpp:221,225,400`
- Create: `tests/unit/test_peer_registry.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)

**Interfaces:**
- Produces: `PeerRegistry::at(size_t) const`, `PeerRegistry::begin()`/`end()` (both const and non-const overloads), all returning/iterating `PeerInfo` by reference. `count()` already exists (`PeerRegistry.h:50`) — unchanged.

- [ ] **Step 1: Write the new test file (fails to compile — `at`/`begin`/`end` don't exist yet)**

```cpp
// tests/unit/test_peer_registry.cpp
#include <gtest/gtest.h>
#include "mesh/PeerRegistry.h"

using namespace lattice::mesh;

static PeerInfo makePeer(uint8_t lastByte) {
  PeerInfo p{};
  memset(p.mac, 0, 6);
  p.mac[5] = lastByte;
  memset(p.publicKey, 0, 32);
  p.lastSeenMs = 0;
  return p;
}

TEST(PeerRegistryTest, CountReflectsAppends) {
  PeerRegistry reg;
  EXPECT_EQ(reg.count(), 0u);
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  EXPECT_EQ(reg.count(), 2u);
}

TEST(PeerRegistryTest, AtReturnsAppendedPeersInOrder) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  EXPECT_EQ(reg.at(0).mac[5], 1);
  EXPECT_EQ(reg.at(1).mac[5], 2);
}

TEST(PeerRegistryTest, IterationVisitsExactlyLiveEntries) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  reg.append(makePeer(3));
  std::vector<uint8_t> seen;
  for (const auto& p : reg) seen.push_back(p.mac[5]);
  EXPECT_EQ(seen, (std::vector<uint8_t>{1, 2, 3}));
}

TEST(PeerRegistryTest, IterationStopsAtCountNotCapacity) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  size_t visited = 0;
  for (const auto& p : reg) { (void)p; ++visited; }
  EXPECT_EQ(visited, 1u);
}

TEST(PeerRegistryTest, ConstIterationWorks) {
  PeerRegistry reg;
  reg.append(makePeer(9));
  const PeerRegistry& constReg = reg;
  size_t visited = 0;
  for (const auto& p : constReg) { (void)p; ++visited; }
  EXPECT_EQ(visited, 1u);
}
```

```
add_unit_test(test_peer_registry     unit/test_peer_registry.cpp)
```
Add this line to `tests/CMakeLists.txt` alongside the other `add_unit_test(...)` calls (after `test_display_manager`, before `test_mbedtls_kat`).

- [ ] **Step 2: Run the new test, confirm it fails to compile**

```bash
cmake --build tests/build --parallel 2 --target test_peer_registry 2>&1 | tail -20
```
Expected: compile error — `PeerRegistry` has no member `at`/`begin`/`end`.

- [ ] **Step 3: Add the iteration API to `PeerRegistry.h` — fields still public at this point**

```cpp
  const PeerInfo& at(size_t i) const { return peerMacs[i]; }
  PeerInfo* begin() { return peerMacs; }
  PeerInfo* end() { return peerMacs + peerCount; }
  const PeerInfo* begin() const { return peerMacs; }
  const PeerInfo* end() const { return peerMacs + peerCount; }
```
Add these 5 lines directly below the existing `size_t count() const { return peerCount; }` in `PeerRegistry.h`.

- [ ] **Step 4: Run the new test, confirm it passes**

```bash
cmake --build tests/build --parallel 2 --target test_peer_registry 2>&1 | tail -20
ctest --test-dir tests/build -R PeerRegistryTest --output-on-failure
```
Expected: 5/5 PASS.

- [ ] **Step 5: Make `peerMacs`/`peerCount` private**

In `PeerRegistry.h`, move `PeerInfo peerMacs[MAX_PEERS]{};` and `size_t peerCount{0};` from the `public:` section (lines 32-33) into the `private:` section (currently just `uint8_t deviceMac[6]{};`).

- [ ] **Step 6: Build everything, fix every compile error at the reach-in sites**

```bash
cmake --build tests/build --parallel 2 2>&1 | grep -E "error|Error"
```

Fix each site:
- `Mesh.h:402-403`:
  ```cpp
  const PeerInfo* getPeerList() const { return peers.begin(); }
  size_t getPeerCount() const { return peers.count(); }
  ```
- `Mesh.cpp` `setupEspNow` (was `for (size_t i = 0; i < peers.peerCount; ++i) { lattice::mesh::crypto::registerPeerWithEspNow(peers.peerMacs[i].mac); }`):
  ```cpp
  for (const auto& p : peers) {
    lattice::mesh::crypto::registerPeerWithEspNow(p.mac);
  }
  ```
- `Mesh.cpp` `broadcastToAllPeers` (was the `peerCount == 0` guard + indexed loop):
  ```cpp
  if (peers.count() == 0) {
    LATTICE_LOGLN("MESH", "WARNING: No peers to broadcast to!", LogLevel::LOG_WARN);
    return;
  }
  for (const auto& p : peers) {
    if (lattice::mac::eq(p.mac, deviceMacAddress))
      continue;
    sendMessage(p.mac, msg);
  }
  ```
- `Mesh.cpp` `relayDownlink` (same indexed-loop shape as `broadcastToAllPeers`, no count==0 guard):
  ```cpp
  for (const auto& p : peers) {
    if (lattice::mac::eq(p.mac, deviceMacAddress))
      continue;
    sendMessage(p.mac, relay);
  }
  ```
- `Mesh.cpp` `addPeer` (was `if (peers.peerCount > before) { ...registerPeerWithEspNow(peers.peerMacs[peers.peerCount - 1].mac); }`):
  ```cpp
  void Mesh::addPeer(const uint8_t* mac) {
    size_t before = peers.count();
    peers.addAndPersist(mac);
    if (peers.count() > before) {
      lattice::mesh::crypto::registerPeerWithEspNow(peers.at(peers.count() - 1).mac);
    }
  }
  ```
- `Mesh.cpp` `registerPeerWithKey` (was `if (peers.peerCount >= MAX_PEERS)`):
  ```cpp
  if (peers.count() >= MAX_PEERS) {
  ```

- [ ] **Step 7: Fix the 3 e2e test sites**

In `tests/e2e/scenarios/test_harness_smoke.cpp`, all 3 occurrences of:
```cpp
[](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.peers.peerCount; });
```
become:
```cpp
[](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.peers.count(); });
```
(lines 221, 225, 400 — `m.peers` itself stays accessible since `peers` is a `Mesh` member under `Mesh`'s own `UNIT_TEST`-conditional public section, unaffected by this task.)

- [ ] **Step 8: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green, no new failures, no removed tests beyond none (this task adds tests, doesn't remove any).

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/PeerRegistry.h firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/e2e/scenarios/test_harness_smoke.cpp tests/unit/test_peer_registry.cpp tests/CMakeLists.txt
git commit -m "refactor(mesh): PeerRegistry encapsulation (finding 5)

peerMacs/peerCount private; add count()/at()/begin()/end() iteration
API. Mesh's 5 direct reach-in sites and getPeerList()/getPeerCount()
now go through it instead of touching the raw array/count."
```

---

### Task 2: `Enrollment` encapsulation + pending-relay queue extraction

**Files:**
- Modify: `firmware/main/src/mesh/Enrollment.h`
- Modify: `firmware/main/src/mesh/Enrollment.cpp`
- Modify: `firmware/main/src/mesh/Mesh.cpp` — `processMasterBeacon:817-819,826-828,840-841`
- Create: `firmware/main/src/mesh/PendingRelayQueue.h`
- Create: `firmware/main/src/mesh/PendingRelayQueue.cpp`
- Modify: `tests/CMakeLists.txt` (add `PendingRelayQueue.cpp` to `FIRMWARE_SOURCES`)
- Create: `tests/unit/test_pending_relay_queue.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)

**Interfaces:**
- Produces: `Enrollment::learnMasterMac(const uint8_t* mac)`, `Enrollment::learnSecondaryMasterMac(const uint8_t* mac)` — both own the memcpy + flag-set + EEPROM-persist triple.
- Produces: `PendingRelayQueue` — `push(const uint8_t* mac, const uint8_t* pubKey)`, `drainTo(DrainFn fn)` where `using DrainFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);`.
- No test files outside this task's own new one need updating — the `#ifdef UNIT_TEST` guard preserves every existing `mesh.enrollment.hasMasterMac`/`knownMasterMac`/etc. direct-field test access unchanged (grep-confirmed ~40 sites across `test_mesh_logic.cpp`, `test_route_report.cpp`, `test_dual_master_e2e.cpp` — all stay compiling as-is).

- [ ] **Step 1: Move the 4 TOFU fields into the existing `UNIT_TEST`-guarded section, add `friend class Mesh;`**

In `Enrollment.h`, remove these 4 lines from the unconditional `public:` section (lines 21-25):
```cpp
  // TOFU state (read by Mesh for beacon/config processing)
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};
```
Add them into the existing `#ifdef UNIT_TEST / public: / #else / private: / #endif` section (currently starting at line 45, where `devicePrivateKey`/`devicePublicKey`/`_enrolled` already live) — same block, just more fields in it:
```cpp
#ifdef UNIT_TEST
public:
#else
private:
#endif
  uint8_t devicePrivateKey[32]{};
  uint8_t devicePublicKey[32]{};
  bool hasMasterMac{false};
  uint8_t knownMasterMac[6]{};
  bool hasMasterMacSecondary{false};
  uint8_t knownMasterMacSecondary[6]{};
  bool _enrolled{false};
```
Add `friend class Mesh;` right after the `class Enrollment {` line (Mesh legitimately needs read access to these fields at 4 remaining sites after this task — see Step 3 — and this is the same pattern the class already uses for `friend class MeshEpochRollbackTest` in `Mesh.h`).

- [ ] **Step 2: Add the two learn methods**

In `Enrollment.h`, add to the public API (near `setRelayFn`):
```cpp
  // Owns the memcpy + flag-set + EEPROM-persist triple for TOFU-learning the
  // (primary/secondary) master MAC — replaces 3 duplicated inline sites in
  // Mesh.cpp and 2 in this file's own processJoinAck() (finding 6).
  void learnMasterMac(const uint8_t* mac);
  void learnSecondaryMasterMac(const uint8_t* mac);
```
In `Enrollment.cpp`, add:
```cpp
void Enrollment::learnMasterMac(const uint8_t* mac) {
  memcpy(knownMasterMac, mac, 6);
  hasMasterMac = true;
  lattice::eeprom::saveKnownMasterMac(knownMasterMac);
}

void Enrollment::learnSecondaryMasterMac(const uint8_t* mac) {
  memcpy(knownMasterMacSecondary, mac, 6);
  hasMasterMacSecondary = true;
  lattice::eeprom::saveKnownMasterMacSecondary(knownMasterMacSecondary);
}
```

- [ ] **Step 3: Replace the 5 write-triple call sites**

In `Enrollment.cpp::processJoinAck` (lines 160-164):
```cpp
  if (!hasMasterMac) {
    learnMasterMac(msg.origin_mac_address);
    LATTICE_LOGLN("MESH", "Master MAC learned and saved (TOFU)", LogLevel::LOG_INFO);
  }
```
(lines 182-188, the secondary-registration block):
```cpp
    if (secondaryRegistered && !hasMasterMacSecondary) {
      learnSecondaryMasterMac(secondaryMasterMac);
    }
```
In `Mesh.cpp::processMasterBeacon`, 3 sites:
- Lines 816-821 (first-beacon TOFU fallback):
  ```cpp
  if (!enrollment.hasMasterMac) {
    enrollment.learnMasterMac(msg.origin_mac_address);
    LATTICE_LOGLN("MESH", "Master MAC learned from first beacon (TOFU fallback)",
                  LogLevel::LOG_INFO);
  }
  ```
- Lines 824-829 (secondary TOFU):
  ```cpp
    if (_dualMasterMode && !enrollment.hasMasterMacSecondary) {
      enrollment.learnSecondaryMasterMac(msg.origin_mac_address);
      LATTICE_LOGLN("MESH", "Secondary master MAC learned (TOFU)", LogLevel::LOG_INFO);
  ```
- Lines 838-841 (stale-master hotswap):
  ```cpp
      LATTICE_LOGLN("MESH", "Stale master — accepting new master MAC", LogLevel::LOG_INFO);
      enrollment.learnMasterMac(msg.origin_mac_address);
  ```

- [ ] **Step 4: Build, confirm only the read sites still compile (via the new friendship), no read-site changes needed**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
```
Expected: clean build. `Mesh.cpp`'s 4 read-only sites (`loadPersistentState:304`, `masterE2EKeys:494`, `processAdapterData:917`, `processAdapterData:1020-1024`, `processMasterBeacon:810-813`) keep compiling unchanged — they're reads of now-private fields, legal via `friend class Mesh;`.

- [ ] **Step 5: Run the existing Enrollment/Mesh-beacon test coverage**

```bash
ctest --test-dir tests/build -R "MeshLogic|RouteReport|DualMaster" --output-on-failure
```
Expected: all green, zero test-file changes needed (confirms the `UNIT_TEST` guard preserved every direct-field test access).

- [ ] **Step 6: Extract `PendingRelayQueue`**

Create `firmware/main/src/mesh/PendingRelayQueue.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

namespace lattice {
namespace mesh {

using PendingRelayDrainFn = void (*)(const uint8_t* mac, const uint8_t* pubKey);

// Bounded, heap-free FIFO of (mac, pubKey) pairs — extracted from Enrollment
// (finding 16), which used this shape for enrollment requests awaiting relay
// to the server. Not templated/generic (YAGNI — this codebase avoids
// per-instantiation template bloat elsewhere, e.g. network/mac_table.h).
class PendingRelayQueue {
public:
  struct Entry {
    uint8_t mac[6];
    uint8_t pubKey[32];
  };

  static constexpr size_t CAPACITY = 4; // matches the old PENDING_RELAY_QUEUE_SIZE

  PendingRelayQueue();

  // Drops (with LOG_WARN) if full. Call only from task context, never ISR —
  // matches the original call site (Mesh::drainRecvQueue, not the RX ISR).
  void push(const uint8_t* mac, const uint8_t* pubKey);

  // Drains every queued entry per call, invoking fn(mac, pubKey) for each.
  void drainTo(PendingRelayDrainFn fn);

private:
  RingbufHandle_t _queue = nullptr;
  StaticRingbuffer_t _queueStruct;
  uint8_t _storage[CAPACITY * sizeof(Entry) + 128];
};

} // namespace mesh
} // namespace lattice
```

Create `firmware/main/src/mesh/PendingRelayQueue.cpp`:
```cpp
#include "PendingRelayQueue.h"
#include "src/logging/Logger.h"

namespace lattice {
namespace mesh {

PendingRelayQueue::PendingRelayQueue() {
  _queue = xRingbufferCreateStatic(sizeof(_storage), RINGBUF_TYPE_NOSPLIT, _storage, &_queueStruct);
}

void PendingRelayQueue::push(const uint8_t* mac, const uint8_t* pubKey) {
  Entry entry;
  memcpy(entry.mac, mac, 6);
  memcpy(entry.pubKey, pubKey, 32);
  if (xRingbufferSend(_queue, &entry, sizeof(entry), 0) != pdTRUE) {
    LATTICE_LOGLN("MESH", "Enrollment relay queue full — dropping request", LogLevel::LOG_WARN);
  }
}

void PendingRelayQueue::drainTo(PendingRelayDrainFn fn) {
  size_t itemSize = 0;
  Entry* entryPtr;
  while ((entryPtr = static_cast<Entry*>(xRingbufferReceive(_queue, &itemSize, 0))) != nullptr) {
    if (itemSize == sizeof(Entry) && fn) {
      fn(entryPtr->mac, entryPtr->pubKey);
    }
    vRingbufferReturnItem(_queue, entryPtr);
  }
}

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 7: Write a direct test for `PendingRelayQueue` before wiring it into `Enrollment`**

```cpp
// tests/unit/test_pending_relay_queue.cpp
#include <gtest/gtest.h>
#include "mesh/PendingRelayQueue.h"

using namespace lattice::mesh;

namespace {
std::vector<std::pair<uint8_t, uint8_t>> g_drained; // (mac[0], pubKey[0]) pairs for identity checks

void recordDrain(const uint8_t* mac, const uint8_t* pubKey) {
  g_drained.emplace_back(mac[0], pubKey[0]);
}
} // namespace

TEST(PendingRelayQueueTest, PushThenDrainDeliversInOrder) {
  g_drained.clear();
  PendingRelayQueue q;
  uint8_t mac1[6] = {1, 0, 0, 0, 0, 0}, pk1[32] = {11};
  uint8_t mac2[6] = {2, 0, 0, 0, 0, 0}, pk2[32] = {22};
  q.push(mac1, pk1);
  q.push(mac2, pk2);
  q.drainTo(recordDrain);
  ASSERT_EQ(g_drained.size(), 2u);
  EXPECT_EQ(g_drained[0], (std::pair<uint8_t, uint8_t>{1, 11}));
  EXPECT_EQ(g_drained[1], (std::pair<uint8_t, uint8_t>{2, 22}));
}

TEST(PendingRelayQueueTest, DrainOnEmptyQueueCallsNothing) {
  g_drained.clear();
  PendingRelayQueue q;
  q.drainTo(recordDrain);
  EXPECT_TRUE(g_drained.empty());
}

TEST(PendingRelayQueueTest, DrainEmptiesTheQueue) {
  g_drained.clear();
  PendingRelayQueue q;
  uint8_t mac[6] = {5, 0, 0, 0, 0, 0}, pk[32] = {55};
  q.push(mac, pk);
  q.drainTo(recordDrain);
  g_drained.clear();
  q.drainTo(recordDrain); // second drain — nothing left
  EXPECT_TRUE(g_drained.empty());
}
```

Add to `tests/CMakeLists.txt`:
```
add_unit_test(test_pending_relay_queue unit/test_pending_relay_queue.cpp)
```
And add `../firmware/main/src/mesh/PendingRelayQueue.cpp` to `FIRMWARE_SOURCES` (next to `Enrollment.cpp`), since `Enrollment.cpp` will depend on it after Step 8.

- [ ] **Step 8: Run the new test**

```bash
cmake --build tests/build --parallel 2 --target test_pending_relay_queue 2>&1 | tail -20
ctest --test-dir tests/build -R PendingRelayQueueTest --output-on-failure
```
Expected: 3/3 PASS.

- [ ] **Step 9: Wire `PendingRelayQueue` into `Enrollment`**

In `Enrollment.h`: remove `PendingRelay` struct, `PENDING_RELAY_QUEUE_SIZE`, `_pendingRelayQueue`/`_pendingRelayQueueStruct`/`_pendingRelayQueueStorage`, and the `enqueuePendingRelay` declaration. Add `#include "PendingRelayQueue.h"` and a member:
```cpp
  PendingRelayQueue _relayQueue;
```
In `Enrollment.cpp`:
- Remove the ring-buffer-creation lines from the constructor (now `PendingRelayQueue`'s own constructor handles this — `Enrollment`'s constructor no longer touches `_pendingRelayQueue` at all).
- `processRequest` (was `enqueuePendingRelay(msg.origin_mac_address, msg.enrollment_public_key);`):
  ```cpp
  void Enrollment::processRequest(const mesh_message& msg) {
    _relayQueue.push(msg.origin_mac_address, msg.enrollment_public_key);
    LATTICE_LOGLN("MESH", "Enrollment request received, deferring relay to loop()",
                  LogLevel::LOG_INFO);
  }
  ```
- Delete `enqueuePendingRelay` entirely (its one line moves into `push`, already done in `PendingRelayQueue.cpp`).
- `setPendingRelay` (was calling `enqueuePendingRelay`):
  ```cpp
  void Enrollment::setPendingRelay(const uint8_t* mac, const uint8_t* pubKey) {
    _relayQueue.push(mac, pubKey);
  }
  ```
- `drainPendingRelay`:
  ```cpp
  void Enrollment::drainPendingRelay() {
    _relayQueue.drainTo(_enrollmentRelayFn);
  }
  ```
  Note: `_enrollmentRelayFn` is `EnrollmentRelayFn` (`void (*)(const uint8_t*, const uint8_t*)`), the exact same signature as `PendingRelayDrainFn` — no adapter needed, pass it directly.

- [ ] **Step 10: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green. Existing enrollment-relay-queue tests (inside `test_mesh_logic.cpp`, if any drive `processRequest`/`drainPendingRelay` through a `Mesh` fixture) keep passing unchanged — behavior is identical, only the implementation moved.

- [ ] **Step 11: Commit**

```bash
git add firmware/main/src/mesh/Enrollment.h firmware/main/src/mesh/Enrollment.cpp firmware/main/src/mesh/Mesh.cpp firmware/main/src/mesh/PendingRelayQueue.h firmware/main/src/mesh/PendingRelayQueue.cpp tests/unit/test_pending_relay_queue.cpp tests/CMakeLists.txt
git commit -m "refactor(mesh): Enrollment encapsulation + PendingRelayQueue extraction (findings 6+16)

TOFU fields moved into the existing UNIT_TEST-guarded section (friend
class Mesh preserves the 4 legitimate production read sites);
learnMasterMac/learnSecondaryMasterMac replace 5 duplicated
memcpy+flag+persist call sites. Pending-relay FIFO extracted into its
own PendingRelayQueue type — Enrollment no longer owns ring-buffer
plumbing directly."
```

---

### Task 3: `ReplayCache` split — narrow to incoming-replay detection, move outbound state to `Mesh`

**Files:**
- Modify: `firmware/main/src/mesh/ReplayCache.h`
- Modify: `firmware/main/src/mesh/Mesh.h`
- Modify: `firmware/main/src/mesh/Mesh.cpp` — `nextSeqGuarded:196-210`, `buildMessage:227-228`, `init:242-246`, `sendEnrollmentRequest` (Mesh.h inline, ~460), `enrollPeer` 4-arg (~1176-1177), `processMasterBeacon:884-893`
- Modify: `tests/unit/test_replay_cache.cpp`
- Create: `tests/unit/test_outbound_sequence_state.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)
- Modify: `tests/e2e/scenarios/test_seq_wrap.cpp` (6 sites)

**Interfaces:**
- Produces: `OutboundSequenceState` (new struct in `ReplayCache.h`) — `init(uint32_t epoch)`, `nextSeq()`, `bumpEpoch(uint32_t)`, `markRelayed(uint32_t epoch, uint16_t seq)`, `wasRelayedBefore(uint32_t epoch, uint16_t seq) const`, public fields `bootEpoch`/`txSeqNum`/`lastRelayedEpoch`/`lastRelayedSeqNum` (kept public — this struct is a plain-data-plus-behavior aggregate the same shape `ReplayCache` itself already is, not a class needing its own encapsulation).
- Changes: `ReplayCache::init()` drops its `uint32_t epoch` parameter (no longer tracks epoch) — becomes `init()` with no args.
- Mesh gains: `OutboundSequenceState txState;` member, and (test-only) `OutboundSequenceState& testTxState() { return txState; }` mirroring the existing `testReplay()`/`testNeighbors()`/`testRoutes()` accessors.

- [ ] **Step 1: Write the new test file for `OutboundSequenceState` (fails to compile — type doesn't exist yet)**

```cpp
// tests/unit/test_outbound_sequence_state.cpp
#include <gtest/gtest.h>
#include "mesh/ReplayCache.h"

using namespace lattice::mesh;

TEST(OutboundSequenceStateTest, InitSetsEpochAndResetsSeq) {
  OutboundSequenceState s;
  s.init(5);
  EXPECT_EQ(s.bootEpoch, 5u);
  EXPECT_EQ(s.txSeqNum, 0u);
}

TEST(OutboundSequenceStateTest, NextSeqIncrements) {
  OutboundSequenceState s;
  s.init(5);
  EXPECT_EQ(s.nextSeq(), 1);
  EXPECT_EQ(s.nextSeq(), 2);
  EXPECT_EQ(s.bootEpoch, 5u);
}

TEST(OutboundSequenceStateTest, BumpEpochUpdatesBootEpoch) {
  OutboundSequenceState s;
  s.init(5);
  s.bumpEpoch(6);
  EXPECT_EQ(s.bootEpoch, 6u);
}

TEST(OutboundSequenceStateTest, WasRelayedBeforeFalseInitially) {
  OutboundSequenceState s;
  s.init(1);
  EXPECT_FALSE(s.wasRelayedBefore(1, 5)); // lastRelayedEpoch/Seq both 0, (1,5) is newer
}

TEST(OutboundSequenceStateTest, MarkRelayedThenWasRelayedBeforeTrueForSameOrOlder) {
  OutboundSequenceState s;
  s.init(1);
  s.markRelayed(1, 5);
  EXPECT_TRUE(s.wasRelayedBefore(1, 5));  // same — not newer
  EXPECT_TRUE(s.wasRelayedBefore(1, 4));  // older seq
  EXPECT_FALSE(s.wasRelayedBefore(1, 6)); // newer seq
  EXPECT_FALSE(s.wasRelayedBefore(2, 0)); // newer epoch
}
```

Add to `tests/CMakeLists.txt`:
```
add_unit_test(test_outbound_sequence_state unit/test_outbound_sequence_state.cpp)
```

- [ ] **Step 2: Run it, confirm compile failure**

```bash
cmake --build tests/build --parallel 2 --target test_outbound_sequence_state 2>&1 | tail -20
```
Expected: `OutboundSequenceState` undeclared.

- [ ] **Step 3: Add `OutboundSequenceState` to `ReplayCache.h`, narrow `ReplayCache` itself**

Remove from the `ReplayCache` struct: `uint32_t bootEpoch{0}; uint16_t txSeqNum{0}; uint32_t lastRelayedEpoch{0}; uint16_t lastRelayedSeqNum{0};` and the `nextSeq()` method. Narrow `init`:
```cpp
  void init() {
    memset(cache, 0, sizeof(cache));
  }
```
Add, in the same file, below the closing brace of `ReplayCache`:
```cpp
// This node's own outbound sequence + relay-dedup bookkeeping — split out of
// ReplayCache (finding 15), which is scoped to incoming-message replay
// detection only. Plain public fields, same as ReplayCache's own cache[] is
// internally: this is a small owned-state aggregate on Mesh, not a class
// needing its own access control.
struct OutboundSequenceState {
  uint32_t bootEpoch{0};
  uint16_t txSeqNum{0};
  uint32_t lastRelayedEpoch{0};
  uint16_t lastRelayedSeqNum{0};

  void init(uint32_t epoch) {
    bootEpoch = epoch;
    txSeqNum = 0;
    lastRelayedEpoch = 0;
    lastRelayedSeqNum = 0;
  }

  uint16_t nextSeq() { return ++txSeqNum; }
  void bumpEpoch(uint32_t newEpoch) { bootEpoch = newEpoch; }

  void markRelayed(uint32_t epoch, uint16_t seq) {
    lastRelayedEpoch = epoch;
    lastRelayedSeqNum = seq;
  }

  bool wasRelayedBefore(uint32_t epoch, uint16_t seq) const {
    bool isNewer = (epoch > lastRelayedEpoch) ||
                   (epoch == lastRelayedEpoch && seq > lastRelayedSeqNum);
    return !isNewer;
  }
};
```

- [ ] **Step 4: Run the new test, confirm it passes**

```bash
cmake --build tests/build --parallel 2 --target test_outbound_sequence_state 2>&1 | tail -20
ctest --test-dir tests/build -R OutboundSequenceStateTest --output-on-failure
```
Expected: 5/5 PASS.

- [ ] **Step 5: Wire `Mesh` to own an `OutboundSequenceState`, update every call site**

In `Mesh.h`: add `OutboundSequenceState txState;` near the existing `ReplayCache replay;` member. Add a test accessor alongside `testReplay()`:
```cpp
  OutboundSequenceState& testTxState() { return txState; }
```

In `Mesh.cpp`:
- `nextSeqGuarded` (was drawing from `replay.nextSeq()`/`replay.bootEpoch`):
  ```cpp
  uint16_t Mesh::nextSeqGuarded() {
    uint16_t seq = txState.nextSeq();
    if (seq == 0) {
      uint32_t epoch = txState.bootEpoch + 1;
      lattice::eeprom::saveBootEpoch(epoch);
      txState.bumpEpoch(epoch);
      seq = txState.nextSeq();
    }
    return seq;
  }
  ```
- `buildMessage` (was `msg.epoch_num = replay.bootEpoch;`): `msg.epoch_num = txState.bootEpoch;`
- `init()` (was `replay.init(epoch);`):
  ```cpp
  replay.init();
  txState.init(epoch);
  ```
- `Mesh.h`'s inline `sendEnrollmentRequest()` (was `enrollment.sendRequest(deviceMacAddress, PROTO_VERSION, replay.bootEpoch, seq);`): `..., txState.bootEpoch, seq);`
- `enrollPeer` 4-arg overload (was `ack.epoch_num = replay.bootEpoch;`): `ack.epoch_num = txState.bootEpoch;`
- `processMasterBeacon` (was the `isNewer`/`replay.lastRelayedEpoch`/`replay.lastRelayedSeqNum` block, lines 884-893):
  ```cpp
    if (txState.wasRelayedBefore(msg.epoch_num, msg.seq_num)) {
      LATTICE_LOGLN("MESH", "Duplicate beacon relay suppressed", LogLevel::LOG_DEBUG);
      return;
    }
    txState.markRelayed(msg.epoch_num, msg.seq_num);
  ```

- [ ] **Step 6: Fix `tests/unit/test_replay_cache.cpp`**

Every `rc.init(N)` call (15 sites) loses its argument: `rc.init(1)` → `rc.init()`, `rc.init(2)` → `rc.init()`, `rc.init(5)` → `rc.init()`.

Delete the `NextSeqIncrements` test entirely (lines 105-111) — `nextSeq()`/`bootEpoch` no longer exist on `ReplayCache`; this behavior is now covered by `OutboundSequenceStateTest.NextSeqIncrements` in the new test file (Step 1).

- [ ] **Step 7: Fix `tests/e2e/scenarios/test_seq_wrap.cpp`**

All 6 occurrences of `mesh.testReplay().txSeqNum` / `mesh.testReplay().bootEpoch` become `mesh.testTxState().txSeqNum` / `mesh.testTxState().bootEpoch` (lines 26, 30, 45, 105, 109, 133).

- [ ] **Step 8: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/ReplayCache.h firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/unit/test_replay_cache.cpp tests/unit/test_outbound_sequence_state.cpp tests/CMakeLists.txt tests/e2e/scenarios/test_seq_wrap.cpp
git commit -m "refactor(mesh): split ReplayCache — outbound state to OutboundSequenceState (finding 15)

ReplayCache narrows to incoming-message replay detection only
(cache[]/isReplay()). bootEpoch/txSeqNum/lastRelayedEpoch/
lastRelayedSeqNum move to a new OutboundSequenceState struct owned by
Mesh directly, with real methods (bumpEpoch/markRelayed/
wasRelayedBefore) instead of raw public-field mutation from Mesh.cpp."
```

---

### Task 4: Extract `MeshTransport`

**Files:**
- Create: `firmware/main/src/mesh/MeshTransport.h`
- Create: `firmware/main/src/mesh/MeshTransport.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove the moved members/methods, add `MeshTransport transport;`
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove moved function bodies, update every caller
- Modify: `firmware/main/src/mesh/MeshCrypto.h` — remove `registerPeerWithEspNow` (moves to `MeshTransport`)
- Modify: `tests/CMakeLists.txt` (add `MeshTransport.cpp` to `FIRMWARE_SOURCES`)

**Interfaces:**
- Produces: `MeshTransport` — owns ESP-NOW setup, the RX ring buffer + trampoline + drain, and send primitives. Public surface: `bool setup(const uint8_t* meshKey, const PeerRegistry& peers)`, `void sendMessage(const uint8_t* target, const mesh_message& msg)`, `void broadcastToAllPeers(const mesh_message& msg, const PeerRegistry& peers, const uint8_t* deviceMac)`, `static bool sendBroadcast(const mesh_message& msg)`, `void drain(MessageHandler handler)` where `using MessageHandler = void (*)(const uint8_t srcMac[6], const mesh_message& msg);`, `void setDrainNotifyHandle(TaskHandle_t)`, `bool injectReceivedMessage(...)` (kept under `#if SIMULATE_MODE`, same as today). Also gains `registerPeerWithEspNow` (moved from `MeshCrypto.h`, finding 19).
- Consumes: nothing from Tasks 1-3 directly in its own logic, but Task 1's `PeerRegistry` iteration API is what `setup()`'s peer-registration loop and `broadcastToAllPeers` use (mirroring what Task 1 already changed inside `Mesh.cpp` — this task relocates that already-updated code).

**Step-by-step:**

- [ ] **Step 1: Create `MeshTransport.h` with the class skeleton and member declarations**

Move these members verbatim from `Mesh.h` (private section) into `MeshTransport`: `static Mesh* instance` becomes `static MeshTransport* instance` (same singleton-trampoline pattern, now scoped to this class); `RECV_QUEUE_SIZE`, `RecvQueueEntry` struct, `recvQueue`/`_recvQueueStruct`/`_recvQueueStorage`, `drainNotifyHandle_`. Declare the moved methods (see Step 2's list) as members of `MeshTransport` instead of `Mesh`.

Add `void registerPeerWithEspNow(const uint8_t mac[6]);` (moved from `MeshCrypto.h` — same body, see Step 5).

The RX trampoline pattern stays identical in shape, just relocated: `static void onDataSentCallback(...)`, `void IRAM_ATTR onDataRecvCallback(...)`, `static void IRAM_ATTR dataRecvTrampoline(...)` — these reference `instance` the same way `Mesh`'s versions did, now `MeshTransport::instance`.

`MessageHandler` (the dispatch callback `drain()` invokes per-message) replaces direct calls into `Mesh::drainRecvQueue`'s inline `switch` — `MeshTransport::drain(MessageHandler handler)` calls `handler(entry.srcMac, entry.msg)` for each dequeued entry instead of dispatching itself; `Mesh` supplies a handler that reproduces `drainRecvQueue`'s existing `switch` body (proto-version check, replay check, `peers.updateLastSeen`, message-type dispatch) — that `switch` stays on `Mesh` since it dispatches into `Mesh`-owned handlers (`enrollment.processRequest`, `processMasterBeacon`, `processAdapterData`, etc.), not into `MeshTransport`.

- [ ] **Step 2: Move these function bodies from `Mesh.cpp` into `MeshTransport.cpp`, unchanged**

`setupWiFi` (279-298, minus the `peers.setDeviceMac`/`readMacAddress` lines which need `deviceMacAddress` — keep those as a small `Mesh::setupRadio()` wrapper that calls `transport.setup(...)` then `readMacAddress()`/`peers.setDeviceMac(...)`, OR pass `deviceMacAddress` in/out — see Step 3 for the exact split), `setupEspNow` (312-344, using Task 1's already-updated peer-iteration loop), `onDataSentCallback` (347-353), `onDataRecvCallback` (355-388), `drainRecvQueue` (390-451, restructured per Step 1's `MessageHandler` split), `dataRecvTrampoline` (453-458), `sendMessage` (460-475), `broadcastToAllPeers` (477-487, using Task 1's peer iteration), `sendBroadcast` (733-740).

- [ ] **Step 3: Resolve `setupWiFi`'s split — MAC address ownership stays on `Mesh`**

`readMacAddress()` writes `Mesh::deviceMacAddress`, which `Mesh` needs for many other things (message building, comparisons throughout `processAdapterData`, etc.) — it does not move to `MeshTransport`. Split `setupWiFi` at the `readMacAddress()`/`peers.setDeviceMac()` line:
```cpp
// MeshTransport.h/.cpp
bool MeshTransport::setup(const uint8_t* meshKey) {
  // ... esp_netif_init through esp_wifi_set_channel, unchanged ...
  return true;
}
```
```cpp
// Mesh.cpp — new small wrapper replacing the old setupWiFi() call site in init()
bool Mesh::setupRadio() {
  if (!transport.setup(meshKey))
    return false;
  readMacAddress();
  peers.setDeviceMac(deviceMacAddress);
  return true;
}
```
`Mesh::init()`'s call to `setupWiFi()` becomes a call to `setupRadio()`. `readMacAddress()` itself (currently `Mesh::readMacAddress`, `Mesh.cpp:74-92`) stays on `Mesh` unchanged.

`setupEspNow` similarly needs `meshKey` (for `esp_now_set_pmk`) and `peers` (for the registration loop) as parameters:
```cpp
bool MeshTransport::setupEspNow(const uint8_t* meshKey, const PeerRegistry& peers) {
  // ... unchanged body, using Task 1's `for (const auto& p : peers)` loop ...
}
```
`Mesh::init()`'s call becomes `transport.setupEspNow(meshKey, peers)`.

- [ ] **Step 4: Wire `drainRecvQueue`'s dispatch split**

`MeshTransport::drain(MessageHandler handler)` replaces the body of the old `drainRecvQueue` from the ring-buffer pop through building `entry`/`itemSize`, then calls `handler(entry.srcMac, entry.msg)` once per dequeued item instead of running the proto-check/replay-check/switch inline. `Mesh` keeps a private method (rename `drainRecvQueue` to keep call sites simple, or introduce `handleReceivedMessage`) with exactly the old proto-version check, replay check, `peers.updateLastSeen` call, and message-type `switch` body — this is the part of the original `drainRecvQueue` that dispatches into `Mesh`-owned handlers, so it stays on `Mesh`:
```cpp
// Mesh.cpp
void Mesh::handleReceivedMessage(const uint8_t srcMac[6], const mesh_message& msg) {
  // proto_version check, replay.isReplay(...) check, peers.updateLastSeen(srcMac),
  // then the switch (msg.message_type) { ... } — all unchanged from the old
  // drainRecvQueue body below its ring-buffer-pop line.
}
```
`Mesh::drain()` (the public one-liner in `Mesh.h`, `void drain() { drainRecvQueue(); }`) becomes `void drain() { transport.drain(&Mesh::handleReceivedMessageTrampoline); }` — `handleReceivedMessage` needs a static trampoline (same `instance`-based pattern) since `MessageHandler` is a plain function pointer, not a bound method:
```cpp
// Mesh.h
static void handleReceivedMessageTrampoline(const uint8_t srcMac[6], const mesh_message& msg) {
  if (instance) instance->handleReceivedMessage(srcMac, msg);
}
```

- [ ] **Step 5: Move `registerPeerWithEspNow` from `MeshCrypto.h` to `MeshTransport`**

Delete `registerPeerWithEspNow` from `MeshCrypto.h` (lines 13-25) — it's peering, not crypto (finding 19). Add the identical body as `MeshTransport::registerPeerWithEspNow`. Update every call site (`Mesh.cpp`: `findNextHopToMaster`, `registerDownlinkPeer` — both stay on `Mesh` until Task 6 moves `registerDownlinkPeer`; `PeerRegistry.cpp` does NOT call it directly, per its own comment "handled by Mesh layer"; `Enrollment.cpp::enrollPeer`) from `lattice::mesh::crypto::registerPeerWithEspNow(mac)` to `transport.registerPeerWithEspNow(mac)`. `Enrollment.cpp` doesn't hold a `Mesh&`/`MeshTransport&` — it currently calls the free function directly; give `Enrollment::enrollPeer` a `RegisterPeerFn`-shaped extra callback parameter for this, OR (simpler, since `MeshTransport::registerPeerWithEspNow` has no `this`-dependent state beyond nothing at all — it's a pure ESP-NOW syscall wrapper) keep it as a **static** method on `MeshTransport` (`static void registerPeerWithEspNow(const uint8_t mac[6]);`) so `Enrollment.cpp` can call `MeshTransport::registerPeerWithEspNow(mac)` directly, exactly as it called the free function before — zero signature-plumbing needed elsewhere.

- [ ] **Step 6: Update `Mesh.h`/`Mesh.cpp` for every other relocated-method caller**

- `Mesh::init()`: `setupWiFi()` → `setupRadio()` (Step 3); `setupEspNow()` → `transport.setupEspNow(meshKey, peers)`.
- `Mesh::sendMessage(...)` calls throughout (`transmitCore`, `broadcastToAllPeers`, `relayDownlink`, `sendDownlinkToNode`, etc.) → `transport.sendMessage(...)`.
- `Mesh::broadcastToAllPeers(...)` calls (`broadcastAdapterData`, `sendDownlinkToNode`'s flood-fallback) → `transport.broadcastToAllPeers(msg, peers, deviceMacAddress)`.
- `Mesh::sendBroadcast(...)` calls (`broadcastMasterBeacon`, `sendEnrollmentRequest` via `Enrollment::sendRequest`, `processJoinAck`'s relay branch, `enrollPeer`, `Mesh::loop()`'s deferred-relay dispatch) → `transport.sendBroadcast(...)`. Note `Enrollment::sendRequest` currently calls `Mesh::sendBroadcast` statically (it holds no `Mesh*`) — give it the same treatment as `registerPeerWithEspNow`: keep `MeshTransport::sendBroadcast` **static** so `Enrollment.cpp` calls `MeshTransport::sendBroadcast(msg)` directly, unchanged in spirit from calling `Mesh::sendBroadcast(msg)` today.
- Remove `Mesh`'s own `sendMessage`/`broadcastToAllPeers`/`sendBroadcast`/`setupWiFi`/`setupEspNow`/`onDataSentCallback`/`onDataRecvCallback`/`dataRecvTrampoline`/`drainRecvQueue`/`RECV_QUEUE_SIZE`/`RecvQueueEntry`/`recvQueue`/`_recvQueueStruct`/`_recvQueueStorage`/`drainNotifyHandle_` from `Mesh.h`/`Mesh.cpp` — they now live only in `MeshTransport`.
- `Mesh::setDrainNotifyHandle(TaskHandle_t handle)` becomes a one-line forward: `transport.setDrainNotifyHandle(handle);`.
- `#if SIMULATE_MODE` `injectReceivedMessage` (`Mesh.h:480-489`) moves to `MeshTransport` (it directly touches `recvQueue`).

- [ ] **Step 7: Add `MeshTransport.cpp` to `FIRMWARE_SOURCES` in `tests/CMakeLists.txt`**

- [ ] **Step 8: Full regression — this is the largest single-step risk in the whole plan, run everything**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green, zero test-file changes needed (this task moves implementation only — `Mesh`'s public API surface, and every `#ifdef UNIT_TEST`-exposed member test fixtures already reach through, is unchanged; `MeshTransport` itself has no test-facing surface change since it's brand new).

If anything fails: this is the task most likely to reveal a missed call site (grep for `recvQueue`, `dataRecvTrampoline`, `onDataRecvCallback`, `RECV_QUEUE_SIZE` across the tree to confirm nothing outside `MeshTransport.{h,cpp}` and `Mesh.h`/`Mesh.cpp` still references the old locations).

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/MeshTransport.h firmware/main/src/mesh/MeshTransport.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp firmware/main/src/mesh/MeshCrypto.h firmware/main/src/mesh/Enrollment.cpp tests/CMakeLists.txt
git commit -m "refactor(mesh): extract MeshTransport (finding 1 job 1; finding 19)

ESP-NOW setup, RX ring buffer + trampoline, and send primitives move
into a new MeshTransport class. registerPeerWithEspNow moves here from
MeshCrypto.h (peering, not crypto). Mesh holds one as a member and
delegates all radio I/O to it."
```

---

### Task 5: Extract `MasterBeacon`

**Files:**
- Create: `firmware/main/src/mesh/MasterBeacon.h`
- Create: `firmware/main/src/mesh/MasterBeacon.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove moved members/methods, add `MasterBeacon beacon;`
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove moved bodies, update callers
- Modify: `tests/CMakeLists.txt` (add `MasterBeacon.cpp` to `FIRMWARE_SOURCES`)

**Interfaces:**
- Produces: `MasterBeacon` — `void broadcast(const mesh_message& beaconTemplate)` (Mesh builds the message via `buildMessage`, `MasterBeacon` just times + sends it — keeps `MasterBeacon` from needing `buildMessage`'s crypto/sequencing dependencies), `void checkTimeout(bool isMaster, MasterInfo& currentMaster, uint8_t lastSeenMasterMac[6])`, `void process(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster, bool dualMasterMode, Enrollment& enrollment, NeighborTable& neighbors, MasterInfo& currentMaster, OutboundSequenceState& txState, mesh_message& relayPendingMsgOut, uint64_t& relayPendingAtOut, bool& relayPendingOut, uint8_t lastSeenMasterMac[6], uint64_t& lastMasterBeaconReceivedMsOut)`.
- Consumes: Task 2's `Enrollment::learnMasterMac`/`learnSecondaryMasterMac`, Task 3's `OutboundSequenceState::wasRelayedBefore`/`markRelayed`, **and Task 4's `handleReceivedMessage`/`MessageHandler` skeleton** (Step 4 below wires `beacon.process(...)` into the `MESH_TYPE_MASTER_BEACON` case of the switch Task 4 creates — this task cannot start until Task 4 is merged).

**Step-by-step:**

- [ ] **Step 1: Create `MasterBeacon.h`/`.cpp`, move `broadcastMasterBeacon`/`checkMasterTimeout` bodies with minimal signature changes**

`broadcastMasterBeacon` (603-618) currently owns `lastBeaconMillis` (the interval timer) — this state moves into `MasterBeacon` as a private member. It builds the beacon via `buildMessage` (a `Mesh`-owned, crypto/sequencing-touching method) — keep `buildMessage`'s call on `Mesh`'s side: `Mesh::broadcastMasterBeacon()` becomes a thin wrapper:
```cpp
// Mesh.cpp
void Mesh::broadcastMasterBeacon() {
  if (!beacon.intervalElapsed())
    return;
  mesh_message msg = buildMessage(adapter_types::UNKNOWN_ADAPTER, nullptr, MESH_TYPE_MASTER_BEACON);
  msg.data[0] = 1;
  msg.hop_count = 0;
  beacon.send(msg, transport);
}
```
```cpp
// MasterBeacon.h/.cpp
class MasterBeacon {
public:
  bool intervalElapsed(); // was the `now - lastBeaconMillis < MASTER_BEACON_INTERVAL_MS` guard; updates lastBeaconMillis as a side effect on true, matching the original's timing semantics exactly
  void send(const mesh_message& msg, MeshTransport& transport) {
    (void)transport.sendBroadcast(msg);
  }
  // ...
private:
  uint64_t lastBeaconMillis{0};
};
```
`checkMasterTimeout` (763-777) is fully self-contained given `isMaster`/`currentMaster`/`lastSeenMasterMac`/`lastMasterBeaconReceivedMs` as parameters — move verbatim as `MasterBeacon::checkTimeout(bool isMaster, MasterInfo& currentMaster, uint8_t lastSeenMasterMac[6])`, with `lastMasterBeaconReceivedMs`/`STALE_MASTER_THRESHOLD_MS` becoming `MasterBeacon`'s own private members (the threshold constant moves too — it's beacon-timing-specific).

- [ ] **Step 2: Move `processMasterBeacon`'s body, threading through the parameters listed in Interfaces above**

The function body (`Mesh.cpp:781-910`, already read in full during planning) moves verbatim except:
- `enrollment.hasMasterMac`/`.knownMasterMac`/`.learnMasterMac(...)`/etc. calls stay exactly as written — `Enrollment&` is passed in, and Task 2's `friend class Mesh;` does NOT extend to `MasterBeacon`, so these become genuine public-API calls (`enrollment.learnMasterMac(...)`, and the 4 *read* sites at lines 810-813 need read accessors — see Step 3).
- `replay.lastRelayedEpoch`/`lastRelayedSeqNum` references become `txState.wasRelayedBefore(...)`/`txState.markRelayed(...)` (already true after Task 3 — no new change here, just confirming the moved code carries Task 3's edit with it).
- `neighbors.observeAndMinDistance(...)` — `NeighborTable&` passed in, call unchanged.
- `lastMasterBeaconReceivedMs`/`lastSeenMasterMac` — passed in as output parameters (or `MasterBeacon` owns `lastMasterBeaconReceivedMs` itself now, alongside `checkTimeout`'s use of it — keep them together on `MasterBeacon` since both methods need the same field).
- `relayPendingMsg`/`relayPendingAt`/`relayPending` stay `Mesh`-owned fields (they're consumed by `Mesh::loop()`'s deferred-relay dispatch, unrelated to beacon processing itself beyond being written here) — passed as output parameters.

- [ ] **Step 3: Resolve the 4 read-only `Enrollment` field accesses `MasterBeacon` needs**

Unlike `Mesh` (which got `friend class Mesh;` in Task 2 for its own permanent read needs), `MasterBeacon` is new code with no such friendship. Add 2 small read accessors to `Enrollment` (alongside `learnMasterMac`/`learnSecondaryMasterMac` from Task 2) rather than granting `MasterBeacon` friendship too (keeps the friend list from growing indefinitely as more collaborators appear):
```cpp
// Enrollment.h, public section
bool hasKnownMaster() const { return hasMasterMac; }
const uint8_t* knownMaster() const { return knownMasterMac; }
bool hasKnownSecondaryMaster() const { return hasMasterMacSecondary; }
const uint8_t* knownSecondaryMaster() const { return knownMasterMacSecondary; }
```
`MasterBeacon`'s moved `processMasterBeacon` body uses `enrollment.hasKnownMaster()`/`enrollment.knownMaster()`/etc. instead of the raw field names at its 4 read sites (810-813); its 3 write sites already call `enrollment.learnMasterMac(...)`/`enrollment.learnSecondaryMasterMac(...)` per Task 2 — no further change needed there.

- [ ] **Step 4: Update `Mesh.h`/`Mesh.cpp` — remove moved members/methods, wire `MasterBeacon beacon;`**

Remove from `Mesh`: `broadcastMasterBeacon`/`checkMasterTimeout`/`processMasterBeacon` bodies (declarations stay as thin wrappers calling into `beacon`, or are removed entirely if `Mesh.h`'s public `broadcastMasterBeacon()`/`checkMasterTimeout()` become direct one-line forwards — keep the public names identical, since `main.cpp` and tests call `mesh.broadcastMasterBeacon()`/`mesh.checkMasterTimeout()` by those names today), `lastBeaconMillis`, `lastMasterBeaconReceivedMs`, `STALE_MASTER_THRESHOLD_MS`. Add `MasterBeacon beacon;` member. `Mesh.h`'s public `broadcastMasterBeacon()`/`checkMasterTimeout()` become:
```cpp
void broadcastMasterBeacon() { /* build via buildMessage, then beacon.send(...) — see Task 5 Step 1 */ }
void checkMasterTimeout() { beacon.checkTimeout(isMaster, currentMaster, lastSeenMasterMac); }
```
`Mesh.cpp`'s `drainRecvQueue`'s (now `handleReceivedMessage`'s, post-Task-4) `MESH_TYPE_MASTER_BEACON` case calls `beacon.process(msg, deviceMacAddress, isMaster, _dualMasterMode, enrollment, neighbors, currentMaster, txState, relayPendingMsg, relayPendingAt, relayPending, lastSeenMasterMac)` instead of `processMasterBeacon(msg)`.

- [ ] **Step 5: Add `MasterBeacon.cpp` to `FIRMWARE_SOURCES`**

- [ ] **Step 6: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green — pay particular attention to `test_dual_master_e2e.cpp` and any beacon-timing/TOFU tests in `test_mesh_logic.cpp`, since this task moves the most security/TOFU-sensitive logic of the three extractions.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/src/mesh/MasterBeacon.h firmware/main/src/mesh/MasterBeacon.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp firmware/main/src/mesh/Enrollment.h tests/CMakeLists.txt
git commit -m "refactor(mesh): extract MasterBeacon (finding 1 job 3)

broadcastMasterBeacon/checkMasterTimeout/processMasterBeacon move into
a new MasterBeacon class. Enrollment gains 4 read accessors
(hasKnownMaster/knownMaster/hasKnownSecondaryMaster/
knownSecondaryMaster) so MasterBeacon can read TOFU state through a
real API instead of needing its own friend declaration."
```

---

### Task 6: Extract `DownlinkRouter`

**Files:**
- Create: `firmware/main/src/mesh/DownlinkRouter.h`
- Create: `firmware/main/src/mesh/DownlinkRouter.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove moved members/methods, add `DownlinkRouter router;`
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove moved bodies, restructure `processAdapterData`'s routing block per the design spec's `classify()` sketch
- Modify: `tests/CMakeLists.txt` (add `DownlinkRouter.cpp` to `FIRMWARE_SOURCES`)

**Interfaces:** exactly the `RouteDecision`/`classify()`/`relayDownlink()`/`registerDownlinkPeer()` signatures locked in `docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md`'s Task 6 section — read that section before starting this task, it has the full class declaration and the exact `processAdapterData` replacement code.

**Step-by-step:**

- [ ] **Step 1: Create `DownlinkRouter.h` with the `RouteDecision` enum and class declaration from the design spec, verbatim**

- [ ] **Step 2: Implement `classify()` in `DownlinkRouter.cpp`, translating `processAdapterData`'s routing block (lines 919-956) into the decision logic**

```cpp
RouteDecision DownlinkRouter::classify(const mesh_message& msg, const uint8_t* deviceMac,
                                       bool isMaster, bool addressedToSelf, bool isBroadcastTarget,
                                       bool addressedToMaster, uint8_t nextHopMacOut[6]) const {
  if (isMaster || addressedToSelf || isBroadcastTarget)
    return RouteDecision::NotRouted;
  if (addressedToMaster) {
    if (msg.hop_count >= lattice::config::MAX_HOPS)
      return RouteDecision::NotRouted; // caller's early-return-on-hop-limit case; see note below
    return RouteDecision::RelayTowardMaster;
  }
  if (msg.route_len > 0 && msg.route_len <= lattice::config::MAX_HOPS) {
    for (uint8_t i = 0; i < msg.route_len; ++i) {
      if (lattice::mac::eq(&msg.route_path[static_cast<size_t>(i) * 6], deviceMac)) {
        if (msg.hop_count >= lattice::config::MAX_HOPS)
          return RouteDecision::NotRouted;
        const uint8_t* next = (i + 1 < msg.route_len)
                                  ? &msg.route_path[static_cast<size_t>(i + 1) * 6]
                                  : msg.target_mac_address;
        memcpy(nextHopMacOut, next, 6);
        return RouteDecision::ForwardOnRoute;
      }
    }
  }
  return RouteDecision::Flood;
}
```

**Important correctness note for the implementer:** the original code's `if (msg.hop_count >= MAX_HOPS) return;` inside the `addressedToMaster` and route-path-match branches is an unconditional early-return-from-`processAdapterData` (drop the frame entirely, do NOT fall through to the security gate below). Mapping both of those to `RouteDecision::NotRouted` is **wrong** — `NotRouted` falls through to the security/local-delivery path in the Task 6 Step 4 `switch`, but the original hop-limit case must drop the frame outright, same as today. Use a 5th enum value for this:
```cpp
enum class RouteDecision { NotRouted, RelayTowardMaster, ForwardOnRoute, Flood, DropHopLimitExceeded };
```
Replace both `return RouteDecision::NotRouted;` lines that follow an `if (msg.hop_count >= lattice::config::MAX_HOPS)` check above with `return RouteDecision::DropHopLimitExceeded;`, and add a `case RouteDecision::DropHopLimitExceeded: return;` arm to the `processAdapterData` `switch` in Step 4 (this is a correction to the design spec's sketch, found while implementing it — update the design spec's code block to match once this task lands, so the two documents stay in sync).

- [ ] **Step 3: Move `relayDownlink`/`registerDownlinkPeer` bodies verbatim**

`relayDownlink` (`Mesh.cpp:1042-1053`, already updated by Task 1's iteration API) becomes `DownlinkRouter::relayDownlink(const mesh_message& msg, const PeerRegistry& peers, const uint8_t* deviceMac, MeshTransport& transport)` — body unchanged except `sendMessage(...)` → `transport.sendMessage(...)`.

`registerDownlinkPeer` (`Mesh.cpp:140-194`) becomes `DownlinkRouter::registerDownlinkPeer(const uint8_t* mac, const PeerRegistry& peers, const MasterInfo& currentMaster)` — body unchanged except every `lattice::mesh::crypto::registerPeerWithEspNow(...)` call becomes `MeshTransport::registerPeerWithEspNow(...)` (static, per Task 4 Step 5) and `downlinkPeerLru`/`downlinkPeerLruCount` become `DownlinkRouter`'s own private members (already declared in the design spec's class sketch).

- [ ] **Step 4: Rewrite `processAdapterData`'s routing block**

Replace lines 919-956 with the `switch` from the design spec (Task 6 section), corrected per Step 2's `DropHopLimitExceeded` addition:
```cpp
uint8_t nextHop[6];
switch (router.classify(msg, deviceMacAddress, isMaster, addressedToSelf, isBroadcastTarget,
                        addressedToMaster, nextHop)) {
case RouteDecision::DropHopLimitExceeded:
  return;
case RouteDecision::RelayTowardMaster: {
  mesh_message relay = msg;
  relay.hop_count++;
  memcpy(relay.last_hop_mac_address, deviceMacAddress, 6);
  transmitCore(static_cast<adapter_types>(relay.data_type), relay.data, MESH_TYPE_ADAPTER_DATA, &relay);
  return;
}
case RouteDecision::ForwardOnRoute: {
  mesh_message fwd = msg;
  fwd.hop_count++;
  router.registerDownlinkPeer(nextHop, peers, currentMaster);
  transport.sendMessage(nextHop, fwd);
  return;
}
case RouteDecision::Flood:
  router.relayDownlink(msg, peers, deviceMacAddress, transport);
  return;
case RouteDecision::NotRouted:
  break; // fall through to the security gate below, unchanged
}
```
Note `msg.hop_count`'s increment happens once per relay/forward as before — `classify()` is read-only (const), so the `relay.hop_count++`/`fwd.hop_count++` mutations stay in `processAdapterData` exactly as in the original, just reorganized around the `switch`.

- [ ] **Step 5: Update the other `registerDownlinkPeer`/`relayDownlink` callers**

`sendDownlinkToNode` (`Mesh.cpp:712`, the `registerDownlinkPeer(msg.route_path);` call) → `router.registerDownlinkPeer(msg.route_path, peers, currentMaster);`. Confirm no other callers exist (`grep -rn "registerDownlinkPeer\|relayDownlink" firmware/main/src`) — `relayDownlink` is also called from `processAdapterData`'s flood-fallback path (already covered by Step 4) and nowhere else per the earlier read-through of `Mesh.cpp`.

- [ ] **Step 6: Remove the moved members from `Mesh.h`, add `DownlinkRouter router;`**

Remove `forwardingPeer`... wait — `forwardingPeer` belongs to `findNextHopToMaster` (uplink routing, stays on `Mesh` — job 2, not job 4), do NOT move it. Only remove `downlinkPeerLru`/`downlinkPeerLruCount` (both move into `DownlinkRouter`) and the `relayDownlink`/`registerDownlinkPeer` method declarations.

- [ ] **Step 7: Add `DownlinkRouter.cpp` to `FIRMWARE_SOURCES`**

- [ ] **Step 8: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green. This task's `DropHopLimitExceeded` correction (Step 2) is the single highest-risk correctness point in this entire plan — if any downlink/multi-hop test starts failing, check first whether a hop-limit-exceeded case is falling through to the security gate instead of dropping.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/DownlinkRouter.h firmware/main/src/mesh/DownlinkRouter.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/CMakeLists.txt
git commit -m "refactor(mesh): extract DownlinkRouter (finding 1 job 4, narrowed; finding 2's routing half)

relayDownlink/registerDownlinkPeer move into a new DownlinkRouter
class, kept crypto-free. processAdapterData's routing-decision block
becomes a switch on DownlinkRouter::classify()'s result; the
security/E2E/config-auth/deliver sequence is untouched. sendRouteReport/
processRouteReport stay on Mesh per the design spec's scope narrowing."
```

- [ ] **Step 10: Sync the design spec's code sketch with the `DropHopLimitExceeded` correction found in Step 2**

```bash
git add docs/superpowers/specs/2026-08-07-phaseB-mesh-cleanup-design.md
git commit -m "docs(phaseB): correct classify() sketch — 5th enum value for hop-limit drops"
```
(Edit the design spec's `RouteDecision` enum and `switch` code block to match what actually shipped in this task, so a future reader isn't misled by the pre-implementation sketch.)

---

### Task 7: `Mesh` becomes thin orchestrator

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h`
- Modify: `firmware/main/src/mesh/Mesh.cpp`

**Interfaces:** none new — this task only removes now-dead wrapper code and folds mesh-key persistence into direct `EepromManager` calls. `Mesh`'s public API is unchanged (every test and `main.cpp` call site keeps working).

- [ ] **Step 1: Fold `loadMeshKeyFromEEPROM`/`saveMeshKeyToEEPROM` into direct calls**

Both methods (`Mesh.cpp:620-651`) do real logic beyond a pure pass-through (`loadMeshKeyFromEEPROM` has the DEV_MODE override + unset-key-fallback logic; `saveMeshKeyToEEPROM` IS a pure one-line pass-through to `lattice::eeprom::saveMeshKey`). Per the design spec's guidance ("keep them if they add real logic beyond the call"): **keep `loadMeshKeyFromEEPROM` as-is** (it has real logic). **Inline `saveMeshKeyToEEPROM`** — replace its 2 call sites (`Mesh.cpp:645` inside `loadMeshKeyFromEEPROM` itself, and any other caller — grep to confirm there's exactly one, inside `loadMeshKeyFromEEPROM`) with a direct `lattice::eeprom::saveMeshKey(meshKey, MESH_KEY_SIZE);` call, and delete `saveMeshKeyToEEPROM`'s declaration/definition.

- [ ] **Step 2: Final dead-code sweep**

```bash
grep -n "^\s*//.*Tiger Style refactor helpers\|^\s*//.*Setup helpers" firmware/main/src/mesh/Mesh.h
```
Remove now-stale section comments in `Mesh.h` that referenced the old inline structure (`// --- Tiger Style refactor helpers ---`, `// Setup helpers (Tiger Style refactor)`) if the methods they labeled have all moved out.

Confirm `Mesh.cpp`'s line count dropped substantially:
```bash
wc -l firmware/main/src/mesh/Mesh.cpp
```
Expected: well under 1382 — no specific target number (this refactor's goal is maintainability, not a size target per the design spec), but a large majority of the original body should have relocated across Tasks 4-6.

- [ ] **Step 3: Full regression, one more time, on the complete Phase B diff**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```
Expected: all green.

- [ ] **Step 4: Commit, push, open PR**

```bash
git add firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp
git commit -m "refactor(mesh): Mesh becomes thin orchestrator (finding 1 jobs 2+5+6)

saveMeshKeyToEEPROM inlined (pure pass-through); stale section
comments removed. Mesh now holds MeshTransport/MasterBeacon/
DownlinkRouter as members and orchestrates message dispatch,
sequence/replay guarding, and the security-critical half of
processAdapterData directly."
git push -u origin docs/phaseB-mesh-cleanup-spec
gh pr create --title "refactor(phaseB): mesh subsystem cleanup" --body "$(cat <<'EOF'
## Summary
- Phase B of the clean-code-refactor umbrella: decompose Mesh.cpp (1382
  lines, 6+ jobs) into MeshTransport/MasterBeacon/DownlinkRouter collaborators
  plus a thin Mesh orchestrator.
- Fixes 3 encapsulation breaks (PeerRegistry, Enrollment, ReplayCache).
- Zero behavior change, zero wire changes.

## Test plan
- [ ] Full unit + e2e regression green after every task (see commit history).
- [ ] CI size delta reported — expect near-zero.
EOF
)"
```

---

## Self-Review Notes

- **Spec coverage:** all 7 findings (1, 2, 5, 6, 15, 16, 19) from the ledger are covered — Tasks 1/2/3 for the 3 encapsulation findings (5, 6, 16, 15), Tasks 4/5/6 for finding 1's 3 extractable jobs + finding 19's opportunistic move + finding 2's routing half, Task 7 for finding 1's remaining jobs 2/5/6 staying on `Mesh`.
- **Placeholder scan:** every task names exact files, exact line ranges (verified against the actual current source read in full during planning), and exact code for all new classes/methods. Tasks 4-6's *relocated* (not new) function bodies are described as "move verbatim" with the source line range given rather than reproduced a second time in this document — the bodies are the same text already in `Mesh.cpp` today; reproducing ~600 lines a second time here would only risk transcription drift against what Tasks 1-3 already changed inline.
- **Type/schema consistency:** `learnMasterMac`/`learnSecondaryMasterMac` (Task 2) are called identically in Task 5's moved `processMasterBeacon` body. `OutboundSequenceState` (Task 3)'s method names (`bumpEpoch`/`markRelayed`/`wasRelayedBefore`) match between its own test file, `nextSeqGuarded`'s Task 3 update, and `processMasterBeacon`'s Task 5 update. `MeshTransport::sendMessage`/`broadcastToAllPeers`/`sendBroadcast`/`registerPeerWithEspNow` signatures are consistent across every caller listed in Tasks 4-6.
- **A genuine mid-plan correction:** Task 6 Step 2 found that the design spec's `RouteDecision` enum (3 values + implicit fall-through) doesn't correctly represent the original code's hop-limit-exceeded drop case, which must bypass the security gate entirely rather than fall through to it. Task 6 Step 10 syncs the design spec with the corrected 5-value enum once the task lands.
