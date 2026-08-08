# Phase B Round 2 — Finish the Mesh SRP Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Finish decomposing `Mesh` (1020 lines after Round 1) down to a genuine orchestrator (~250-350 lines) by extracting everything remaining that isn't pure boot/dispatch orchestration — including the previously-protected `processAdapterData` security half and `sendRouteReport`/`processRouteReport` — with zero behavior change.

**Architecture:** 7 tasks, strictly sequential (unlike Round 1's Tasks 1-3, nothing here is safe to parallelize — every task narrows `Mesh.cpp` for the next one). Two new free-function headers (`E2EKeyLookup.h`, `PeerEnrollment.h`) matching this codebase's established convention (`mac_table.h`, `MacEq.h`, `mem.h`, `hw_mac.h`). Four new stateful classes (`UplinkRouter`, `MeshMessenger`, `RouteReportHandler`, `FrameAuthorizer`). `OutboundSequenceState` (from Round 1 Task 3) grows two more methods.

**Tech Stack:** Same as Round 1 — C++17, ESP-IDF (host-mocked for tests), GoogleTest, CMake, `UNIT_TEST` compile-definition idiom for white-box test access.

## Global Constraints

- **No wire-format changes, no behavior changes anywhere.** Same as Round 1 — pure structural refactor.
- **Firmware-only.**
- **Full unit + e2e regression required after every task.** `cmake --build tests/build --parallel 2` (this machine OOMs on full-parallel builds) then `ctest --test-dir tests/build --output-on-failure --label-exclude e2e` and `--label-regex e2e`. Baseline going in: 272 unit + 41 e2e.
- **Every task that creates a `.cpp` under `firmware/main/src` MUST add it to BOTH `tests/CMakeLists.txt`'s `FIRMWARE_SOURCES` AND `firmware/main/CMakeLists.txt`'s `SRCS` list in the same commit.** Round 1's final whole-branch review caught a real gap where `PendingRelayQueue.cpp` was registered in only the first — this is now a required step in every task below, not an afterthought.
- **`processAdapterData`'s security logic (Task 13) and `sendRouteReport`/`processRouteReport` (Task 12) move verbatim — same checks, same ordering, same comments, same comparison operators.** Relocating security-sensitive code is fine (that's the whole point of this round); rewriting its logic while relocating it is not. Every task below that touches security-sensitive code says so explicitly and names the verification method (extract original body from the diff, normalize only the documented renames, confirm exact match — same method Round 1's Task 4/5/6 reviewers used).
- **CI size delta reported per PR.** Expect near-zero — Round 2's two new headers are `inline` free functions (same flash profile as today's inline calls), the four new classes add no virtual dispatch and no new heap allocation.

## Sequencing

```
Task 8  (OutboundSequenceState grows)      ── self-contained
Task 9  (E2EKeyLookup.h)                   ── self-contained
Task 10 (PeerEnrollment.h + UplinkRouter)  ── self-contained
Task 11 (MeshMessenger)                    ── needs Tasks 8, 9, 10
Task 12 (RouteReportHandler)               ── needs Tasks 8, 9, 11
Task 13 (FrameAuthorizer)                  ── needs Task 9
Task 14 (Mesh thin orchestrator, round 2)  ── needs Tasks 8-13
```

All 7 run sequentially in this session (no worktree parallelism this round — see the design spec's Round 2 section for why Tasks 8-10 aren't split into parallel worktrees despite being independent of each other: they all touch `Mesh.cpp` at overlapping locations once call sites update, same conflict-risk reasoning as Round 1's Tasks 1-3, and this round's tasks are individually smaller so the sequential cost is lower than the coordination overhead).

---

### Task 8: `OutboundSequenceState` grows `nextSeqGuarded`/`checkEpochRollback`

**Files:**
- Modify: `firmware/main/src/mesh/ReplayCache.h`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `_lastSealedEpoch`/`_lastSealedSeq` fields, remove `nextSeqGuarded`/`_checkEpochRollback` declarations
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove the two function bodies, update every call site (`buildMessage`, `transmitCore`'s seal guard call, `init`'s epoch handling stays as-is since it calls `txState.init(epoch)` already from Round 1, `sendDownlinkToNode`'s seal guard, `enrollPeer`'s ACK sequencing)
- Create: `tests/unit/test_outbound_sequence_state.cpp` already exists from Round 1 Task 3 — extend it with new tests for `nextSeqGuarded`/`checkEpochRollback`

**Interfaces:**
- Produces: `OutboundSequenceState::nextSeqGuarded()` (was `Mesh::nextSeqGuarded`), `OutboundSequenceState::checkEpochRollback(uint32_t epoch, uint16_t seq)` (was `Mesh::_checkEpochRollback`).
- Consumes: nothing new.

- [ ] **Step 1: Write failing tests for the wrap-and-rollback behavior**

Add to `tests/unit/test_outbound_sequence_state.cpp`:

```cpp
TEST(OutboundSequenceStateTest, NextSeqGuardedBumpsEpochOnWrap) {
  OutboundSequenceState s;
  s.init(5);
  s.txSeqNum = 0xFFFE; // one below wrap
  EXPECT_EQ(s.nextSeqGuarded(), 0xFFFF);
  EXPECT_EQ(s.bootEpoch, 5u);
  uint16_t wrapped = s.nextSeqGuarded(); // this draw wraps 0xFFFF -> 0 -> guarded redraw
  EXPECT_EQ(wrapped, 1); // epoch bumped, fresh sequence starts at 1
  EXPECT_EQ(s.bootEpoch, 6u);
}

TEST(OutboundSequenceStateTest, CheckEpochRollbackAcceptsFirstCall) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5); // must not fail — first call always passes
}

TEST(OutboundSequenceStateTest, CheckEpochRollbackAcceptsStrictlyNewer) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5);
  s.checkEpochRollback(1, 6);  // same epoch, higher seq — ok
  s.checkEpochRollback(2, 0);  // higher epoch — ok
}
```

`CheckEpochRollbackRejectsRollback` (the failure case) cannot be asserted
via `EXPECT_DEATH`-style testing in this codebase's `UNIT_TEST` build —
confirm by reading `error/Error.h`/`error/ErrorCore.h`: under `UNIT_TEST`,
`err::fail` throws `FatalError` (per this project's standing convention —
see the plan's Global Constraints references elsewhere in this repo's
other phase plans). Add:

```cpp
TEST(OutboundSequenceStateTest, CheckEpochRollbackThrowsOnRollback) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5);
  EXPECT_THROW(s.checkEpochRollback(1, 4), lattice::err::FatalError);
}
```

(Confirm the exact exception type name by grepping `error/Error.h` for
`FatalError` before writing this — use whatever the real type is if it
differs from this name.)

- [ ] **Step 2: Run the new tests, confirm they fail to compile**

```bash
cmake --build tests/build --parallel 2 --target test_outbound_sequence_state 2>&1 | tail -20
```
Expected: `nextSeqGuarded`/`checkEpochRollback` undeclared.

- [ ] **Step 3: Add both methods to `OutboundSequenceState` in `ReplayCache.h`**

```cpp
  uint16_t nextSeqGuarded() {
    uint16_t seq = nextSeq();
    if (seq == 0) {
      uint32_t epoch = bootEpoch + 1;
      lattice::eeprom::saveBootEpoch(epoch);
      bumpEpoch(epoch);
      seq = nextSeq();
    }
    return seq;
  }

  void checkEpochRollback(uint32_t epoch, uint16_t seq) {
    if (_lastSealedEpoch == UINT32_MAX) {
      _lastSealedEpoch = epoch;
      _lastSealedSeq = seq;
      return;
    }
    if (epoch > _lastSealedEpoch) {
      _lastSealedEpoch = epoch;
      _lastSealedSeq = seq;
      return;
    }
    if (epoch == _lastSealedEpoch && seq > _lastSealedSeq) {
      _lastSealedSeq = seq;
      return;
    }
    lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO, lattice::core::ModuleDigit::MESH, 1,
                       "AEAD epoch rollback — refusing seal");
  }

private:
  uint32_t _lastSealedEpoch = UINT32_MAX;
  uint16_t _lastSealedSeq = 0;
```

(The `private:` section is new for `OutboundSequenceState` — it was
previously an all-public struct; adding these two fields as private with
the two methods above as its only mutators is itself a small encapsulation
improvement, consistent with Round 1 Task 3's spirit, not scope creep —
note it in the commit message but don't treat it as a separate task.)

Add includes to `ReplayCache.h`: `#include "src/persistence/EepromManager.h"`,
`#include "src/error/Error.h"`, `#include "src/error/ErrorCore.h"`.

- [ ] **Step 4: Run the new tests, confirm they pass**

```bash
cmake --build tests/build --parallel 2 --target test_outbound_sequence_state 2>&1 | tail -20
ctest --test-dir tests/build -R OutboundSequenceStateTest --output-on-failure
```

- [ ] **Step 5: Remove the old methods from `Mesh`, update every call site**

In `Mesh.h`: remove `_lastSealedEpoch`/`_lastSealedSeq` fields, remove
`nextSeqGuarded()`/`_checkEpochRollback(...)` declarations.

In `Mesh.cpp`:
- `buildMessage` (line ~146-166, `msg.seq_num = nextSeqGuarded();`): →
  `msg.seq_num = txState.nextSeqGuarded();`
- `transmitCore` (line ~313-377, `_checkEpochRollback(msg.epoch_num, msg.seq_num);`):
  → `txState.checkEpochRollback(msg.epoch_num, msg.seq_num);`
- `sendDownlinkToNode` (line ~474-517, same `_checkEpochRollback` call): →
  `txState.checkEpochRollback(msg.epoch_num, msg.seq_num);`
- `Mesh.h`'s inline `sendEnrollmentRequest()` (calls `nextSeqGuarded()`
  directly): → `txState.nextSeqGuarded()`
- `enrollPeer` 4-arg overload (line ~799-846, `ack.seq_num = nextSeqGuarded();`):
  → `ack.seq_num = txState.nextSeqGuarded();`

Grep to confirm no other call sites: `grep -rn "nextSeqGuarded\|_checkEpochRollback" firmware/main/src tests`
(everything should now say `txState.nextSeqGuarded()`/`txState.checkEpochRollback(...)`).

- [ ] **Step 6: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

- [ ] **Step 7: Commit**

```bash
git add firmware/main/src/mesh/ReplayCache.h firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/unit/test_outbound_sequence_state.cpp
git commit -m "refactor(mesh): OutboundSequenceState grows nextSeqGuarded/checkEpochRollback (round 2 task 8)

Both methods moved verbatim from Mesh; OutboundSequenceState's 4 data
fields plus these 2 sealed-epoch fields are now private with real
mutator methods instead of a bare-public struct."
```

---

### Task 9: `E2EKeyLookup.h` (new file, free functions)

**Files:**
- Create: `firmware/main/src/mesh/E2EKeyLookup.h`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `masterE2EKeys`/`peerE2EKeys` declarations
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove the two bodies, update every call site
- Create: `tests/unit/test_e2e_key_lookup.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test — header-only file, no `.cpp` to add to `FIRMWARE_SOURCES`)

**Interfaces:**
- Produces: `lattice::mesh::masterE2EKeys(const MasterInfo&, PeerRegistry&, Enrollment&, E2EKeyStore&, const uint8_t**, const uint8_t**)`, `lattice::mesh::peerE2EKeys(const uint8_t*, PeerRegistry&, Enrollment&, E2EKeyStore&, const uint8_t**, const uint8_t**)`.

- [ ] **Step 1: Create `E2EKeyLookup.h`**

```cpp
#pragma once
#include <cstdint>
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

// Bridges MasterInfo + PeerRegistry + Enrollment + E2EKeyStore — free functions
// rather than a method on any one of the 4 collaborators (round 2 task 9), matching
// this codebase's existing convention for cross-cutting utilities (network/mac_table.h,
// network/MacEq.h, network/mem.h, network/hw_mac.h). Adding these as methods on
// E2EKeyStore would give it an artificial dependency on PeerRegistry/Enrollment it
// otherwise deliberately doesn't have.

// Returns k_up/k_down for the current master (leaf side); false if not enrolled
// or master pubkey unknown.
inline bool masterE2EKeys(const MasterInfo& currentMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, const uint8_t** kUp,
                          const uint8_t** kDown) {
  if (!enrollment.hasKnownMaster())
    return false;
  PeerInfo* master = peers.find(currentMaster.mac);
  if (!master)
    return false;
  return e2eKeys.getKeys(master->mac, enrollment.getPrivateKey(), master->publicKey, kUp, kDown);
}

// Returns keys for an enrolled origin peer (master side); false if unknown peer.
inline bool peerE2EKeys(const uint8_t* originMac, PeerRegistry& peers, Enrollment& enrollment,
                        E2EKeyStore& e2eKeys, const uint8_t** kUp, const uint8_t** kDown) {
  PeerInfo* peer = peers.find(originMac);
  if (!peer)
    return false;
  return e2eKeys.getKeys(peer->mac, enrollment.getPrivateKey(), peer->publicKey, kUp, kDown);
}

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Write direct tests before wiring into `Mesh.cpp`**

```cpp
// tests/unit/test_e2e_key_lookup.cpp
#include <gtest/gtest.h>
#include "mesh/E2EKeyLookup.h"

using namespace lattice::mesh;

TEST(E2EKeyLookupTest, MasterE2EKeysFalseWhenNoMasterKnown) {
  MasterInfo currentMaster{};
  PeerRegistry peers;
  Enrollment enrollment;
  E2EKeyStore e2eKeys;
  const uint8_t *kUp, *kDown;
  EXPECT_FALSE(masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown));
}

TEST(E2EKeyLookupTest, PeerE2EKeysFalseWhenPeerUnknown) {
  PeerRegistry peers;
  Enrollment enrollment;
  E2EKeyStore e2eKeys;
  uint8_t unknownMac[6] = {9, 9, 9, 9, 9, 9};
  const uint8_t *kUp, *kDown;
  EXPECT_FALSE(peerE2EKeys(unknownMac, peers, enrollment, e2eKeys, &kUp, &kDown));
}
```

(Full success-path coverage — a known peer with a real derived key —
already exists indirectly via `test_mesh_logic.cpp`'s existing
`Mesh`-level fixture tests, which will continue to exercise this code
through `Mesh`'s call sites after Step 4 rewires them; these 2 new direct
tests cover the two failure branches, which is what's cheap and valuable
to test in isolation.)

Add to `tests/CMakeLists.txt`:
```
add_unit_test(test_e2e_key_lookup     unit/test_e2e_key_lookup.cpp)
```

- [ ] **Step 3: Run the new tests, confirm they pass**

```bash
cmake --build tests/build --parallel 2 --target test_e2e_key_lookup 2>&1 | tail -20
ctest --test-dir tests/build -R E2EKeyLookupTest --output-on-failure
```

- [ ] **Step 4: Remove the old methods from `Mesh`, update every call site**

In `Mesh.h`: remove `masterE2EKeys`/`peerE2EKeys` declarations. Add
`#include "E2EKeyLookup.h"`.

In `Mesh.cpp`:
- `transmitCore` (line ~313-377, `if (!masterE2EKeys(&kUp, &kDown) || ...)`):
  → `if (!lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown) || ...)`
- `sendDownlinkToNode` (line ~474-517, `if (!peerE2EKeys(destMac, &kUp, &kDown) || ...)`):
  → `if (!lattice::mesh::peerE2EKeys(destMac, peers, enrollment, e2eKeys, &kUp, &kDown) || ...)`

Grep to confirm no other call sites: `grep -rn "masterE2EKeys\|peerE2EKeys" firmware/main/src tests`
— every remaining hit should be the qualified `lattice::mesh::` form or a
comment.

- [ ] **Step 5: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/mesh/E2EKeyLookup.h firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/unit/test_e2e_key_lookup.cpp tests/CMakeLists.txt
git commit -m "refactor(mesh): extract E2EKeyLookup.h free functions (round 2 task 9)

masterE2EKeys/peerE2EKeys moved verbatim into a new free-function
header, matching mac_table.h/MacEq.h/mem.h/hw_mac.h's existing
convention rather than coupling E2EKeyStore to PeerRegistry/Enrollment."
```

---

### Task 10: `PeerEnrollment.h`/`.cpp` (free functions) + `UplinkRouter` (new class)

**Files:**
- Create: `firmware/main/src/mesh/PeerEnrollment.h`, `firmware/main/src/mesh/PeerEnrollment.cpp`
- Create: `firmware/main/src/mesh/UplinkRouter.h`, `firmware/main/src/mesh/UplinkRouter.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `findNextHopToMaster`, `registerPeerWithKey`, `addPeer` declarations (keep `registerPeerWithKeyTrampoline`, becomes a 3-line static forwarding to the free function); remove `forwardingPeer[6]`, `nextHopScratch` fields; add `UplinkRouter uplinkRouter;` member
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove the moved bodies, update call sites
- Modify: `tests/CMakeLists.txt` (add `PeerEnrollment.cpp` and `UplinkRouter.cpp` to `FIRMWARE_SOURCES`)
- Modify: `firmware/main/CMakeLists.txt` (add both `.cpp` files to `SRCS`)
- Create: `tests/unit/test_uplink_router.cpp`

**Interfaces:**
- Produces: `lattice::mesh::registerPeerWithKey(const uint8_t*, const uint8_t*, bool, PeerRegistry&, Enrollment&, bool)`, `lattice::mesh::addPeer(const uint8_t*, PeerRegistry&)`, `lattice::mesh::dispatchJoinAck(const mesh_message&, const uint8_t*, bool, Enrollment&, RegisterPeerFn)`. `UplinkRouter::findNextHopToMaster(const MasterInfo&, PeerRegistry&, NeighborTable&, const uint8_t*, uint64_t) -> PeerInfo*`.

- [ ] **Step 1: Create `PeerEnrollment.h`/`.cpp`**

```cpp
// mesh/PeerEnrollment.h
#pragma once
#include <cstdint>
#include "Enrollment.h"
#include "PeerRegistry.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {

// Bridges PeerRegistry + Enrollment + MeshTransport::registerPeerWithEspNow (round 2
// task 10) — free functions, same reasoning as E2EKeyLookup.h (task 9).

// Add or rekey a peer with a known public key. allowRekey=false (over-the-air
// JOIN_ACK) never replaces an established key; allowRekey=true (server-approved
// enrollment) may. Returns false if the registry is full and the peer could not
// be added.
bool registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey,
                         PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode);

// Optional UI/app-triggered peer add.
void addPeer(const uint8_t* mac, PeerRegistry& peers);

// Outer JOIN_ACK dispatch: relay (via MeshTransport::sendBroadcast) if not addressed
// to this device; else delegate to enrollment.processJoinAck(...).
void dispatchJoinAck(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                     Enrollment& enrollment, RegisterPeerFn registerFn);

} // namespace mesh
} // namespace lattice
```

`PeerEnrollment.cpp` bodies move verbatim from `Mesh.cpp`'s current
implementations:
- `registerPeerWithKey` (currently `Mesh.cpp:756-794`) — only change:
  `lattice::mesh::crypto::registerPeerWithEspNow` calls inside stay as
  `enrollment.enrollPeer(mac, publicKey32, nullptr, dualMasterMode)`
  unchanged (that call already routes through `Enrollment`'s own
  ESP-NOW registration internally, per Round 1's `MeshTransport::registerPeerWithEspNow`
  static method).
- `addPeer` (currently `Mesh.cpp:748-755`) — verbatim, `peers.addAndPersist(mac)`
  + conditional `MeshTransport::registerPeerWithEspNow(...)` call.
- `dispatchJoinAck` (currently `Mesh.cpp:714-743`, the body of
  `Mesh::processJoinAck`) — verbatim; the relay branch's `sendBroadcast(relay)`
  call becomes `MeshTransport::sendBroadcast(relay)` (static, per Round 1
  Task 4).

- [ ] **Step 2: Write a direct test for `addPeer`/`registerPeerWithKey` before wiring in**

```cpp
// tests/unit/test_peer_enrollment.cpp — new file
#include <gtest/gtest.h>
#include "mesh/PeerEnrollment.h"

using namespace lattice::mesh;

TEST(PeerEnrollmentTest, AddPeerAppendsToRegistry) {
  PeerRegistry peers;
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  addPeer(mac, peers);
  EXPECT_EQ(peers.count(), 1u);
  EXPECT_NE(peers.find(mac), nullptr);
}

TEST(PeerEnrollmentTest, RegisterPeerWithKeyRejectsRekeyOfEstablishedKey) {
  PeerRegistry peers;
  Enrollment enrollment;
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  uint8_t key1[32]; memset(key1, 0xAA, 32);
  uint8_t key2[32]; memset(key2, 0xBB, 32);
  ASSERT_TRUE(registerPeerWithKey(mac, key1, /*allowRekey=*/false, peers, enrollment, false));
  ASSERT_TRUE(registerPeerWithKey(mac, key2, /*allowRekey=*/false, peers, enrollment, false));
  // Established (non-zero) key must not be replaced when allowRekey is false.
  EXPECT_EQ(memcmp(peers.find(mac)->publicKey, key1, 32), 0);
}
```

Register in `tests/CMakeLists.txt`:
```
add_unit_test(test_peer_enrollment    unit/test_peer_enrollment.cpp)
```
Add `../firmware/main/src/mesh/PeerEnrollment.cpp` to `FIRMWARE_SOURCES`.

- [ ] **Step 3: Run the new tests, confirm they pass**

```bash
cmake --build tests/build --parallel 2 --target test_peer_enrollment 2>&1 | tail -20
ctest --test-dir tests/build -R PeerEnrollmentTest --output-on-failure
```

- [ ] **Step 4: Create `UplinkRouter.h`/`.cpp`**

```cpp
// mesh/UplinkRouter.h
#pragma once
#include <cstdint>
#include "NeighborTable.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

// Uplink mirror of DownlinkRouter's forwarding-peer bookkeeping (round 2 task 10) —
// here a single-slot "LRU" since a node only ever forwards uplink to one next hop
// at a time. Owns findNextHopToMaster, moved verbatim from Mesh.
class UplinkRouter {
public:
  // Returns a pointer to a scratch PeerInfo (not a `peers` member — mirrors the
  // original nextHopScratch pattern) representing the chosen next hop, or nullptr
  // if no route exists. Side effect: registers the chosen hop with
  // MeshTransport::registerPeerWithEspNow and evicts the prior forwardingPeer from
  // ESP-NOW if it changed and isn't itself an enrolled peer or the current master.
  PeerInfo* findNextHopToMaster(const MasterInfo& currentMaster, PeerRegistry& peers,
                                NeighborTable& neighbors, const uint8_t* deviceMac,
                                uint64_t nowMs);

private:
  uint8_t forwardingPeer[6]{};
  PeerInfo nextHopScratch{};
};

} // namespace mesh
} // namespace lattice
```

`UplinkRouter.cpp`'s `findNextHopToMaster` body moves verbatim from
`Mesh.cpp:84-130` (already read in full during Round 1 planning) — only
the parameter threading changes (implicit `currentMaster`/`peers`/
`neighbors`/`deviceMacAddress` member access becomes explicit parameters,
`esp_timer_get_time()/1000ULL` stays inline since it's a free ESP-IDF
call not a collaborator), and `lattice::mesh::crypto::registerPeerWithEspNow`
becomes `MeshTransport::registerPeerWithEspNow` (static).

- [ ] **Step 5: Write a direct test for `UplinkRouter::findNextHopToMaster`**

```cpp
// tests/unit/test_uplink_router.cpp
#include <gtest/gtest.h>
#include "mesh/UplinkRouter.h"

using namespace lattice::mesh;

TEST(UplinkRouterTest, ReturnsNullWhenNoMasterKnown) {
  UplinkRouter router;
  MasterInfo currentMaster{}; currentMaster.distance = 0xFF;
  PeerRegistry peers;
  NeighborTable neighbors;
  uint8_t deviceMac[6] = {1, 1, 1, 1, 1, 1};
  EXPECT_EQ(router.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac, 1000), nullptr);
}

TEST(UplinkRouterTest, ReturnsDirectPeerWhenInRangeAtDistanceOne) {
  UplinkRouter router;
  MasterInfo currentMaster{};
  memset(currentMaster.mac, 0xAA, 6);
  currentMaster.distance = 1;
  PeerRegistry peers;
  PeerInfo master{};
  memcpy(master.mac, currentMaster.mac, 6);
  master.lastSeenMs = 1000;
  peers.append(master);
  NeighborTable neighbors;
  uint8_t deviceMac[6] = {1, 1, 1, 1, 1, 1};
  PeerInfo* hop = router.findNextHopToMaster(currentMaster, peers, neighbors, deviceMac, 1000);
  ASSERT_NE(hop, nullptr);
  EXPECT_EQ(memcmp(hop->mac, currentMaster.mac, 6), 0);
}
```

(These two tests cover the "no route" and "direct single-hop" cases — the
multi-hop-via-`NeighborTable` case already has coverage through
`test_mesh_logic.cpp`'s existing multi-hop fixture tests, which continue
exercising this code through `Mesh`'s call sites after Step 7 rewires
them.)

Register in `tests/CMakeLists.txt`:
```
add_unit_test(test_uplink_router      unit/test_uplink_router.cpp)
```
Add `../firmware/main/src/mesh/UplinkRouter.cpp` to `FIRMWARE_SOURCES`.

- [ ] **Step 6: Run the new tests, confirm they pass**

```bash
cmake --build tests/build --parallel 2 --target test_uplink_router 2>&1 | tail -20
ctest --test-dir tests/build -R UplinkRouterTest --output-on-failure
```

- [ ] **Step 7: Remove the old methods/fields from `Mesh`, update every call site**

In `Mesh.h`: remove `findNextHopToMaster`, `registerPeerWithKey`,
`addPeer` declarations; remove `forwardingPeer[6]`, `nextHopScratch`
fields. Add `UplinkRouter uplinkRouter;` member. Keep
`registerPeerWithKeyTrampoline` but rewrite its body:
```cpp
static bool registerPeerWithKeyTrampoline(const uint8_t* mac, const uint8_t* publicKey32) {
  return lattice::mesh::registerPeerWithKey(mac, publicKey32, /*allowRekey=*/false,
                                            instance->peers, instance->enrollment,
                                            instance->_dualMasterMode);
}
```
Add public forwarding methods for `Mesh`'s existing public API
(`addPeer`, called by app/UI code per `Mesh.h`'s existing comment "optional,
can be used in your app/UI"):
```cpp
void addPeer(const uint8_t* mac) { lattice::mesh::addPeer(mac, peers); }
```

In `Mesh.cpp`:
- `processJoinAck` (currently `Mesh.cpp:714-743`) — replace entire body
  with: `lattice::mesh::dispatchJoinAck(msg, deviceMacAddress, isMaster, enrollment, &Mesh::registerPeerWithKeyTrampoline);`
  (Keep the `Mesh::processJoinAck` wrapper method itself — it's `handleReceivedMessage`'s
  `MESH_TYPE_JOIN_ACK` dispatch target, just becomes a 1-line forward.)
- Every `findNextHopToMaster()` call (in `transmitCore`, `relayEnrollmentUplink`,
  `sendRouteReport` — grep to find all, there are at least 3):
  → `uplinkRouter.findNextHopToMaster(currentMaster, peers, neighbors, deviceMacAddress, static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL)`

Grep to confirm no other call sites:
`grep -rn "findNextHopToMaster\|Mesh::registerPeerWithKey\b\|Mesh::addPeer\b" firmware/main/src tests`

- [ ] **Step 8: Full regression**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -30
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

- [ ] **Step 9: Commit**

```bash
git add firmware/main/src/mesh/PeerEnrollment.h firmware/main/src/mesh/PeerEnrollment.cpp firmware/main/src/mesh/UplinkRouter.h firmware/main/src/mesh/UplinkRouter.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/unit/test_peer_enrollment.cpp tests/unit/test_uplink_router.cpp tests/CMakeLists.txt firmware/main/CMakeLists.txt
git commit -m "refactor(mesh): extract PeerEnrollment.h + UplinkRouter (round 2 task 10)

registerPeerWithKey/addPeer/dispatchJoinAck move to free functions
(peer-approval orchestration bridging PeerRegistry+Enrollment).
findNextHopToMaster + forwardingPeer LRU-of-one move to a new
UplinkRouter class, mirroring DownlinkRouter's shape."
```

---

### Task 11: `MeshMessenger` (new class — largest single Round 2 extraction)

**Files:**
- Create: `firmware/main/src/mesh/MeshMessenger.h`, `firmware/main/src/mesh/MeshMessenger.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `buildMessage`, `transmitDispatch` declarations (keep `transmit`/`transmitSelfOriginated`/`broadcastAdapterData(Static)`/`sendDownlinkToNode(Static)`/`enrollPeer`(2 overloads) as thin public forwards — same public API surface, callers like `main.cpp`/`Adapter`/tests are unaffected); add `MeshMessenger messenger;` member
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove the 10 moved bodies, rewrite the kept public methods as 1-line forwards
- Modify: `tests/CMakeLists.txt` (add `MeshMessenger.cpp` to `FIRMWARE_SOURCES`)
- Modify: `firmware/main/CMakeLists.txt` (add `MeshMessenger.cpp` to `SRCS`)

**Interfaces:**
- Produces: `MeshMessenger` owning `buildMessage`, `transmitCore`, `transmitDispatch`, `broadcastAdapterData`, `sendDownlinkToNode`, `enrollPeer` (2 overloads), `relayEnrollmentUplink`. Exact signatures below — each takes the collaborators it needs as parameters (`MeshTransport&`, `OutboundSequenceState&`, `PeerRegistry&`, `Enrollment&`, `E2EKeyStore&`, `UplinkRouter&`, `RouteTable*`, `MasterInfo&`, `const uint8_t* deviceMac`, `bool isMaster`) — this is the class with the most dependencies in the whole plan, by design (see design spec's Task 11 note).
- Consumes: Task 8 (`txState.nextSeqGuarded()`/`checkEpochRollback`), Task 9 (`lattice::mesh::masterE2EKeys`/`peerE2EKeys`), Task 10 (`uplinkRouter.findNextHopToMaster`).

- [ ] **Step 1: Create `MeshMessenger.h` with the full class declaration**

```cpp
// mesh/MeshMessenger.h
#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "../../lib/lattice-protocol/c/message_types.h"
#include "src/adapter/Adapter.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "MeshTransport.h"
#include "NeighborTable.h"
#include "PeerRegistry.h"
#include "ReplayCache.h"
#include "RouteTable.h"
#include "UplinkRouter.h"

namespace lattice {
namespace mesh {

using ::mesh_message;
using ::MeshMessageType;
using lattice::adapter::adapter_types;

// Owns outbound message construction and dispatch (round 2 task 11) — the single
// place "how does this node send something" lives. Depends on more collaborators
// than any other class in this plan by design: everything a send needs to thread
// through (sequencing, E2E crypto, routing) converges here.
class MeshMessenger {
public:
  static constexpr uint8_t PROTO_VERSION = 5; // mirrors Mesh::PROTO_VERSION

  mesh_message buildMessage(adapter_types type, const uint8_t* data, MeshMessageType msgType,
                            const uint8_t* deviceMac, const MasterInfo& currentMaster,
                            OutboundSequenceState& txState);

  void transmitCore(const adapter_types type, const uint8_t* data, MeshMessageType msgType,
                    const mesh_message* msgOverride, bool isMaster, const uint8_t* deviceMac,
                    MasterInfo& currentMaster, OutboundSequenceState& txState, PeerRegistry& peers,
                    Enrollment& enrollment, E2EKeyStore& e2eKeys, UplinkRouter& uplinkRouter,
                    NeighborTable& neighbors, MeshTransport& transport);

  void transmitDispatch(const adapter_types type, const uint8_t* data, bool selfOriginated,
                        bool isMaster, /* ...same params as transmitCore, plus: */
                        ExternalRecvCallback externalRecvCallback, const uint8_t* deviceMac,
                        MasterInfo& currentMaster, OutboundSequenceState& txState,
                        PeerRegistry& peers, Enrollment& enrollment, E2EKeyStore& e2eKeys,
                        UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                        MeshTransport& transport);

  void broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally,
                            const uint8_t* deviceMac, const MasterInfo& currentMaster,
                            OutboundSequenceState& txState, PeerRegistry& peers,
                            MeshTransport& transport, ExternalRecvCallback externalRecvCallback);

  void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data,
                          bool isMaster, const uint8_t* deviceMac, MasterInfo& currentMaster,
                          OutboundSequenceState& txState, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, RouteTable* routes,
                          MeshTransport& transport);

  void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                  const uint8_t* secondaryPubKey32, const uint8_t* deviceMac,
                  OutboundSequenceState& txState, Enrollment& enrollment, MeshTransport& transport);

  void relayEnrollmentUplink(const mesh_message& msg, const uint8_t* deviceMac,
                             MasterInfo& currentMaster, OutboundSequenceState& txState,
                             PeerRegistry& peers, Enrollment& enrollment, E2EKeyStore& e2eKeys,
                             UplinkRouter& uplinkRouter, NeighborTable& neighbors,
                             MeshTransport& transport);
};

} // namespace mesh
} // namespace lattice
```

(`ExternalRecvCallback` — introduce a `using ExternalRecvCallback =
std::function<void(const mesh_message&)>;` typedef, matching `Mesh`'s
existing member type, in `MeshMessenger.h` or a small shared header if
Task 12/13 need the same typedef — check `Mesh.h`'s current
`externalRecvCallback` member type before finalizing.)

**Note on parameter count:** several of these signatures are long. This is
the design's known tradeoff (see design spec's Task 11 note) — `Mesh`'s
own call sites become the thing that's threading the parameters, which is
exactly what an orchestrator does. Do not "simplify" by having
`MeshMessenger` hold a `Mesh&` back-pointer instead — that would violate
the plan's crypto-free/no-back-pointer discipline that's held for every
other collaborator in this plan.

- [ ] **Step 2: Move all 10 function bodies into `MeshMessenger.cpp` verbatim**

Each body is unchanged from what was read in full during Round 1 planning
(`Mesh.cpp` line numbers as of the current HEAD — re-verify with `grep -n`
before starting, since Task 8/9/10 shifted them):
- `buildMessage` (~146-166)
- `transmitCore` (~313-377) — internal calls become
  `txState.checkEpochRollback(...)`, `lattice::mesh::masterE2EKeys(...)`,
  `uplinkRouter.findNextHopToMaster(...)`, `transport.sendMessage(...)`
- `transmitDispatch` (~378-385)
- `broadcastAdapterData` (~449-473) — internal `broadcastToAllPeers` call
  becomes `transport.broadcastToAllPeers(msg, peers, deviceMac)`
- `sendDownlinkToNode` (~474-517) — internal calls become
  `txState.checkEpochRollback(...)`, `lattice::mesh::peerE2EKeys(...)`,
  `transport.sendMessage(...)`, `transport.broadcastToAllPeers(...)`
- `enrollPeer` 4-arg overload (~799-846) — internal calls become
  `txState.nextSeqGuarded()`, `transport.sendBroadcast(...)`
- `relayEnrollmentUplink` (~693-713) — internal calls become
  `uplinkRouter.findNextHopToMaster(...)`, `transmitCore(...)` (self-call,
  now a method call on the same `MeshMessenger` instance)

The 2-arg `enrollPeer` overload (`Mesh.cpp:795-798`, a 1-line forward to
the 4-arg version with `nullptr, nullptr`) moves too, same shape.

`transmit`/`transmitSelfOriginated` (the **static** entry points,
`Mesh.cpp:386-401`) **stay on `Mesh`**, not `MeshMessenger` — they're
static trampolines through `Mesh::instance` (required since `Adapter`
holds a plain function-pointer member, per `Mesh.h`'s existing comment on
`Adapter::TransmitPtr`), and routing them through `Mesh::instance ->
messenger.transmitDispatch(...)` is simpler than giving `MeshMessenger`
its own second singleton. Rewrite as:
```cpp
void Mesh::transmit(const adapter_types type, const uint8_t* data) {
  if (!instance) { LATTICE_LOGLN("MESH", "transmit() called before init", LogLevel::LOG_WARN); return; }
  instance->messenger.transmitDispatch(type, data, /*selfOriginated=*/false, instance->isMaster,
                                       instance->externalRecvCallback, instance->deviceMacAddress,
                                       instance->currentMaster, instance->txState, instance->peers,
                                       instance->enrollment, instance->e2eKeys,
                                       instance->uplinkRouter, instance->neighbors,
                                       instance->transport);
}
```
(`transmitSelfOriginated` identical, `selfOriginated=true`.)

`broadcastAdapterDataStatic`/`sendDownlinkToNodeStatic` (`Mesh.cpp:518-528`)
stay on `Mesh` for the same static-trampoline reason, forwarding into
`instance->messenger.broadcastAdapterData(...)`/`sendDownlinkToNode(...)`.

- [ ] **Step 3: Wire `Mesh`'s remaining public methods as thin forwards**

`Mesh.h`'s public `broadcastAdapterData`/`sendDownlinkToNode` (the
non-static instance methods `main.cpp`/`SerialAdapter` call) become:
```cpp
void broadcastAdapterData(adapter_types type, const uint8_t* data, bool deliverLocally = false) {
  messenger.broadcastAdapterData(type, data, deliverLocally, deviceMacAddress, currentMaster,
                                 txState, peers, transport, externalRecvCallback);
}
void sendDownlinkToNode(const uint8_t* destMac, adapter_types type, const uint8_t* data) {
  messenger.sendDownlinkToNode(destMac, type, data, isMaster, deviceMacAddress, currentMaster,
                               txState, peers, enrollment, e2eKeys, routes.get(), transport);
}
void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32) {
  enrollPeer(mac, publicKey32, nullptr, nullptr);
}
void enrollPeer(const uint8_t* mac, const uint8_t* publicKey32, const uint8_t* secondaryMac,
                const uint8_t* secondaryPubKey32) {
  messenger.enrollPeer(mac, publicKey32, secondaryMac, secondaryPubKey32, deviceMacAddress,
                       txState, enrollment, transport);
}
```
`Mesh::sendEnrollmentRequest()`'s call chain
(`enrollment.sendRequest(...)`) is unaffected — it doesn't go through
`transmitCore`.

`relayEnrollmentUplink`'s caller (`handleReceivedMessage`'s
`MESH_TYPE_ENROLLMENT` case, `!isMaster` branch) becomes:
```cpp
messenger.relayEnrollmentUplink(msg, deviceMacAddress, currentMaster, txState, peers, enrollment,
                                e2eKeys, uplinkRouter, neighbors, transport);
```

- [ ] **Step 4: Full regression — this is the highest-risk task in Round 2, same caution as Round 1's Task 4**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

If anything fails: grep for any remaining direct `Mesh::buildMessage`/
`Mesh::transmitCore`/`Mesh::transmitDispatch`/`Mesh::broadcastAdapterData`/
`Mesh::sendDownlinkToNode`/`Mesh::enrollPeer`/`Mesh::relayEnrollmentUplink`
reference outside `Mesh.h`'s new thin forwards.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/src/mesh/MeshMessenger.h firmware/main/src/mesh/MeshMessenger.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/CMakeLists.txt firmware/main/CMakeLists.txt
git commit -m "refactor(mesh): extract MeshMessenger (round 2 task 11)

buildMessage/transmitCore/transmitDispatch/broadcastAdapterData/
sendDownlinkToNode/enrollPeer/relayEnrollmentUplink move into a new
MeshMessenger class -- the largest single extraction in round 2.
Mesh's static transmit()/transmitSelfOriginated() trampolines stay on
Mesh (Adapter holds a plain function pointer to them) and forward into
messenger.transmitDispatch()."
```

---

### Task 12: `RouteReportHandler` (new class, security-sensitive)

**Files:**
- Create: `firmware/main/src/mesh/RouteReportHandler.h`, `firmware/main/src/mesh/RouteReportHandler.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `sendRouteReport`/`processRouteReport` declarations; add `RouteReportHandler routeReportHandler;` member
- Modify: `firmware/main/src/mesh/Mesh.cpp` — remove both bodies, update call sites (`Mesh::loop()`'s periodic `sendRouteReport()` call, `handleReceivedMessage`'s `MESH_TYPE_ROUTE_REPORT` case)
- Modify: `tests/CMakeLists.txt` / `firmware/main/CMakeLists.txt` (register `RouteReportHandler.cpp`)

**Interfaces:**
- Produces: `RouteReportHandler::sendRouteReport(...) -> bool`, `RouteReportHandler::processRouteReport(...)`. Both take `MeshMessenger&` (for the send/relay calls both functions make internally) plus whatever collaborators the original bodies touched.

**Move verbatim. Do not restructure the chain-MAC verify loop, the E2E
open sequence, or the route_len bounds checks while relocating — same
discipline as Round 1's protected security code.** Verify by extracting
the original body from this task's diff, normalizing only the documented
collaborator-access renames, and confirming an exact match — the same
method Round 1's Task 5/6 reviewers used on `MasterBeacon::process` and
`DownlinkRouter::classify()`.

- [ ] **Step 1: Create `RouteReportHandler.h`**

```cpp
// mesh/RouteReportHandler.h
#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "MeshMessenger.h"
#include "PeerRegistry.h"
#include "RouteTable.h"
#include "UplinkRouter.h"

namespace lattice {
namespace mesh {

// Route-report protocol handling — send + process, including chain-MAC
// verification (issue #44) and E2E open/seal. Moved verbatim from Mesh (round 2
// task 12); same protected-security-code discipline as Mesh's remaining
// processAdapterData half (task 13) and DownlinkRouter's hop-limit distinction.
class RouteReportHandler {
public:
  bool sendRouteReport(bool isMaster, UplinkRouter& uplinkRouter, MasterInfo& currentMaster,
                       PeerRegistry& peers, NeighborTable& neighbors, Enrollment& enrollment,
                       E2EKeyStore& e2eKeys, const uint8_t* deviceMac,
                       OutboundSequenceState& txState, MeshMessenger& messenger,
                       MeshTransport& transport);

  void processRouteReport(const mesh_message& msg, bool isMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, RouteTable* routes,
                          const uint8_t* deviceMac, MasterInfo& currentMaster,
                          OutboundSequenceState& txState, MeshMessenger& messenger,
                          MeshTransport& transport, ExternalRecvCallback externalRecvCallback);
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Move both bodies verbatim into `RouteReportHandler.cpp`**

`sendRouteReport` (currently `Mesh.cpp:847-858`) — internal
`findNextHopToMaster()` becomes `uplinkRouter.findNextHopToMaster(...)`,
`transmitCore(...)` becomes `messenger.transmitCore(...)` with the full
parameter list Task 11 defined.

`processRouteReport` (currently `Mesh.cpp:859-980`, ~122 lines — the
chain-MAC verify loop, E2E open, `RouteTable::record` call, the relay
branch's chain-MAC extend) — internal `peerE2EKeys(...)`/`masterE2EKeys(...)`
become `lattice::mesh::peerE2EKeys(...)`/`lattice::mesh::masterE2EKeys(...)`
(Task 9), `transmitCore(...)` becomes `messenger.transmitCore(...)`.
**Every comparison, every bounds check, every `memcmp`, every
`routemac::buildHopContext`/`chainStep` call stays byte-for-byte
identical** — this function's comments document the exact attack this
logic closes (issue #44 route-path forgery); read them again before
moving anything and preserve every one.

- [ ] **Step 3: Update call sites**

`Mesh.h`: remove `sendRouteReport`/`processRouteReport` declarations, add
`RouteReportHandler routeReportHandler;` member.

`Mesh.cpp`'s `loop()` (periodic call): `if (sendRouteReport())` becomes
`if (routeReportHandler.sendRouteReport(isMaster, uplinkRouter, currentMaster, peers, neighbors, enrollment, e2eKeys, deviceMacAddress, txState, messenger, transport))`.

`handleReceivedMessage`'s `MESH_TYPE_ROUTE_REPORT` case: `processRouteReport(msg);`
becomes `routeReportHandler.processRouteReport(msg, isMaster, peers, enrollment, e2eKeys, routes.get(), deviceMacAddress, currentMaster, txState, messenger, transport, externalRecvCallback);`.

- [ ] **Step 4: Full regression — pay particular attention to route-report e2e scenarios**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

Run `test_route_report.cpp` and any e2e route-report scenario explicitly
and read the output, not just the pass/fail count:
```bash
ctest --test-dir tests/build -R RouteReport --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add firmware/main/src/mesh/RouteReportHandler.h firmware/main/src/mesh/RouteReportHandler.cpp firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp tests/CMakeLists.txt firmware/main/CMakeLists.txt
git commit -m "refactor(mesh): extract RouteReportHandler (round 2 task 12)

sendRouteReport/processRouteReport move verbatim -- chain-MAC verify
loop, E2E open, RouteTable recording all unchanged in shape. Same
protected-security-code discipline as processAdapterData's remaining
half and DownlinkRouter's hop-limit distinction."
```

---

### Task 13: `FrameAuthorizer` (new class, most security-sensitive)

**Files:**
- Create: `firmware/main/src/mesh/FrameAuthorizer.h`, `firmware/main/src/mesh/FrameAuthorizer.cpp`
- Modify: `firmware/main/src/mesh/Mesh.h` — add `FrameAuthorizer frameAuthorizer;` member
- Modify: `firmware/main/src/mesh/Mesh.cpp` — rewrite `processAdapterData`'s post-`DownlinkRouter::classify()` tail to call `frameAuthorizer.authorize(...)`
- Modify: `tests/CMakeLists.txt` / `firmware/main/CMakeLists.txt` (register `FrameAuthorizer.cpp`)
- Create: `tests/unit/test_frame_authorizer.cpp`

**Interfaces:**
- Produces: `enum class AuthResult { Rejected, Authorized };` and `FrameAuthorizer::authorize(const mesh_message&, bool isMaster, bool addressedToSelf, PeerRegistry&, Enrollment&, E2EKeyStore&, mesh_message& openedOut) -> AuthResult`.

**This is the plan's most security-sensitive extraction — move verbatim,
same discipline as Task 12, PLUS this task requires new direct tests
(see Step 4) rather than relying on integration coverage alone. This is
the proactive application of the lesson Round 1's Task 6 learned the hard
way (`DropHopLimitExceeded` shipped with zero direct tests until the final
review caught it).**

- [ ] **Step 1: Read the current security-half body before touching anything**

```bash
grep -n "void Mesh::processAdapterData" firmware/main/src/mesh/Mesh.cpp
```
Read the full function. The block to extract starts after
`DownlinkRouter::classify()`'s `switch` (Round 1 Task 6) — i.e. everything
from the `case RouteDecision::NotRouted: break;` fallthrough point onward:
the master-not-self-addressed sealed-type gate, both E2E-open branches,
both config-opcode gates, ending just before `if (externalRecvCallback)
externalRecvCallback(opened);`.

- [ ] **Step 2: Create `FrameAuthorizer.h`**

```cpp
// mesh/FrameAuthorizer.h
#pragma once
#include <cstdint>
#include "../../lib/lattice-protocol/c/mesh_message.h"
#include "E2EKeyStore.h"
#include "Enrollment.h"
#include "PeerRegistry.h"

namespace lattice {
namespace mesh {

enum class AuthResult { Rejected, Authorized };

// Authorization decision + E2E open for an inbound ADAPTER_DATA frame (round 2 task
// 13) -- moved verbatim from Mesh::processAdapterData's security half. The E2E-open
// step is part of the authorization question (an unopened, still-sealed frame isn't
// yet known-authentic), not separable the way DownlinkRouter's crypto-free routing
// decision was. Mesh keeps only local-delivery dispatch after this returns.
class FrameAuthorizer {
public:
  // openedOut is written only when the result is Authorized.
  AuthResult authorize(const mesh_message& msg, bool isMaster, bool addressedToSelf,
                      PeerRegistry& peers, Enrollment& enrollment, E2EKeyStore& e2eKeys,
                      mesh_message& openedOut);
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 3: Move the body verbatim into `FrameAuthorizer.cpp`**

Preserve the exact sequence: master-not-self-addressed sealed-type gate
(`LATTICE_LOGLN` + `return AuthResult::Rejected` in place of the original's
bare `return;`) → E2E open master-side (`peerE2EKeys` via
`lattice::mesh::peerE2EKeys`, Task 9) → E2E open node-side
(`masterE2EKeys` via `lattice::mesh::masterE2EKeys`) → config-opcode
unopened/broadcast gate → config-opcode non-master-origin gate → `return
AuthResult::Authorized;` with `openedOut = opened;` set before the final
return (not after — `opened` must be fully populated, including whichever
E2E-open branch ran, or left as a copy of `msg` unchanged if neither
branch fired, exactly matching the original's fall-through-to-local-delivery
semantics for non-sealed message types).

Every `return;` in the original that meant "drop this frame" becomes
`return AuthResult::Rejected;`. Every log line, every comment — including
the ones documenting the specific forged-broadcast-frame attack this logic
closes — moves unchanged.

- [ ] **Step 4: Write the dedicated tests this task's design requires**

```cpp
// tests/unit/test_frame_authorizer.cpp
#include <gtest/gtest.h>
#include "mesh/FrameAuthorizer.h"

using namespace lattice::mesh;

// Helper to build a minimal sealed ADAPTER_DATA mesh_message for each scenario —
// implementer fills in exact field values matching what buildMessage()/sealPayload()
// would produce; see test_mesh_logic.cpp's existing message-construction helpers
// for the established pattern in this test file.

TEST(FrameAuthorizerTest, RejectsSealedTypeNotAddressedToSelfAtMaster) {
  // isMaster=true, addressedToSelf=false, message_type=MESH_TYPE_ADAPTER_DATA (sealed).
  // Expect: AuthResult::Rejected. This is the "stale self-echo or forgery" gate.
}

TEST(FrameAuthorizerTest, RejectsWhenMasterSideE2EOpenFails) {
  // isMaster=true, addressedToSelf=true, message_type=MESH_TYPE_ADAPTER_DATA, but
  // peerE2EKeys() has no key for the origin (unknown/unenrolled peer).
  // Expect: AuthResult::Rejected.
}

TEST(FrameAuthorizerTest, RejectsWhenNodeSideE2EOpenFails) {
  // isMaster=false, addressedToSelf=true, message_type=MESH_TYPE_ADAPTER_DATA, but
  // masterE2EKeys() unavailable (not enrolled).
  // Expect: AuthResult::Rejected.
}

TEST(FrameAuthorizerTest, RejectsConfigOpcodeViaUnopenedBroadcastPath) {
  // The specific forged-broadcast attack the original comments document: a plaintext
  // BROADCAST-target (FF:FF) ADAPTER_DATA frame with data[0] == OP_CONFIG_SET, never
  // addressedToSelf, so never opened. Expect: AuthResult::Rejected — this is the
  // regression test for the exact vulnerability Mesh.cpp's comments describe
  // ("one plaintext RF frame could reboot/reconfigure any node").
}

TEST(FrameAuthorizerTest, RejectsConfigOpcodeFromNonMasterOrigin) {
  // Sealed, opened successfully, opcode is OP_CONFIG_SET/OP_NODE_ID_SET, but
  // origin_mac doesn't match enrollment's known primary or secondary master.
  // Expect: AuthResult::Rejected.
}

TEST(FrameAuthorizerTest, AuthorizesLegitimateMasterSideUplink) {
  // isMaster=true, addressedToSelf=true, valid E2E keys, non-config opcode.
  // Expect: AuthResult::Authorized, openedOut populated with the decrypted payload.
}

TEST(FrameAuthorizerTest, AuthorizesLegitimateNodeSideDownlink) {
  // isMaster=false, addressedToSelf=true, valid E2E keys, non-config opcode.
  // Expect: AuthResult::Authorized, openedOut populated with the decrypted payload.
}
```

Fill in each test body using the exact `mesh_message` field values and
`E2EKeyStore`/`Enrollment`/`PeerRegistry` setup `test_mesh_logic.cpp`'s
existing `processAdapterData`-level tests already use for the equivalent
scenarios (grep `test_mesh_logic.cpp` for `AdapterDataRelayTest` and
similar fixtures from Round 1 Task 6 for the established construction
pattern) — do not invent new message-building helpers if the file already
has ones that fit.

Register in `tests/CMakeLists.txt`:
```
add_unit_test(test_frame_authorizer   unit/test_frame_authorizer.cpp)
```
Add `../firmware/main/src/mesh/FrameAuthorizer.cpp` to `FIRMWARE_SOURCES`.

- [ ] **Step 5: Run the new tests, confirm all 7 pass**

```bash
cmake --build tests/build --parallel 2 --target test_frame_authorizer 2>&1 | tail -30
ctest --test-dir tests/build -R FrameAuthorizerTest --output-on-failure
```

- [ ] **Step 6: Rewrite `processAdapterData`'s tail to call `authorize()`**

```cpp
// Mesh.cpp, replacing everything from the NotRouted fallthrough point
// through the config-opcode gates
mesh_message opened;
if (frameAuthorizer.authorize(msg, isMaster, addressedToSelf, peers, enrollment, e2eKeys, opened)
    == AuthResult::Rejected) {
  return;
}
if (externalRecvCallback)
  externalRecvCallback(opened);
if (isBroadcastTarget && !isMaster) {
  router.relayDownlink(msg, peers, deviceMacAddress, transport);
}
```

(The final broadcast-re-relay block is unchanged from Round 1 — confirm
it's still exactly this shape before and after this edit.)

- [ ] **Step 7: Full regression — this is the plan's highest-stakes verification point**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

Additionally, verify the new tests are load-bearing the same way Round 1
Task 6's fix-round did: temporarily invert one rejection condition (e.g.
change `RejectsConfigOpcodeViaUnopenedBroadcastPath`'s underlying gate to
not fire), confirm the corresponding test fails, then restore. Document
this verification in the commit message or task report — don't just claim
the tests are load-bearing, prove it the way Round 1's Task 6 fix did.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/src/mesh/FrameAuthorizer.h firmware/main/src/mesh/FrameAuthorizer.cpp firmware/main/src/mesh/Mesh.cpp firmware/main/src/mesh/Mesh.h tests/unit/test_frame_authorizer.cpp tests/CMakeLists.txt firmware/main/CMakeLists.txt
git commit -m "refactor(mesh): extract FrameAuthorizer (round 2 task 13)

processAdapterData's security half (gate check, E2E open both
directions, config-opcode authorization) moves verbatim into a new
FrameAuthorizer class -- the previously-protected block from round 1.
7 new direct tests cover every rejection/authorization path,
including the specific forged-broadcast-config-opcode attack the
original comments document. Mesh keeps only local delivery dispatch."
```

---

### Task 14: `Mesh` becomes thin orchestrator, round 2 (final)

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h`
- Modify: `firmware/main/src/mesh/Mesh.cpp`

**Interfaces:** none new — final wiring pass only.

- [ ] **Step 1: Confirm every Round 2 collaborator is wired as a `Mesh` member**

`txState` (Task 8, already a member since Round 1 Task 3), `uplinkRouter`
(Task 10), `messenger` (Task 11), `routeReportHandler` (Task 12),
`frameAuthorizer` (Task 13) — read `Mesh.h`'s member list and confirm all
5 are present and constructed correctly (default-constructible, no
constructor-injection needed per this plan's designs).

- [ ] **Step 2: Final dead-code sweep**

```bash
grep -n "forwardingPeer\|nextHopScratch\|_lastSealedEpoch\|_lastSealedSeq" firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp
```
Expected: no hits (all moved in Tasks 8/10). Remove any stale section
comments referencing moved code (same style as Round 1 Task 7's sweep —
grep for `Mesh.cpp`/`Mesh::` references in comments across files this
plan touched: `NeighborTable.h`, `test_mesh_logic.cpp`,
`project_config.h`, and any others surfaced during Tasks 8-13).

Confirm `Mesh.cpp`'s line count:
```bash
wc -l firmware/main/src/mesh/Mesh.cpp firmware/main/src/mesh/Mesh.h
```
Expected: `Mesh.cpp` in the ~150-250 range (down from 1020), `Mesh.h` grew
somewhat from the 5 new member declarations plus the thin forwarding
methods from Tasks 10/11 — total `Mesh.{h,cpp}` combined should be well
under the original single-file 1382-line benchmark this whole plan started
from.

- [ ] **Step 3: Full regression, one more time, on the complete Round 2 diff**

```bash
cmake --build tests/build --parallel 2 2>&1 | tail -60
ctest --test-dir tests/build --output-on-failure --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --label-regex e2e
```

- [ ] **Step 4: Commit, push**

```bash
git add firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp
git commit -m "refactor(mesh): Mesh becomes thin orchestrator, round 2 (final)

All 5 round-2 collaborators (OutboundSequenceState's growth, plus
UplinkRouter/MeshMessenger/RouteReportHandler/FrameAuthorizer)
confirmed wired. Dead-code sweep. Mesh.cpp now genuinely an
orchestrator: init/setupRadio/loadPersistentState/loop plus
handleReceivedMessage's dispatch switch and thin forwards."
git push
```

(No new PR — this continues PR #97 on the same branch, unless the
controller decides otherwise at execution time based on the state of that
PR when this task is picked up.)

---

## Self-Review Notes

- **Spec coverage:** all 7 design-spec tasks (8-14) are covered — 2 new
  free-function headers, 4 new stateful classes, `OutboundSequenceState`'s
  growth, and the final wiring pass.
- **Placeholder scan:** every task names exact files, exact current line
  numbers (re-verify with `grep -n` at execution time since each task
  shifts subsequent line numbers), and exact code for all new
  classes/methods/free-functions. Verbatim-moved bodies are described by
  citation + "preserve exact X/Y/Z" rather than reproduced a second time,
  same convention Round 1 used for its own large moves.
- **Type/schema consistency:** `MeshMessenger`'s method signatures (Task
  11) are the ones Task 12 (`RouteReportHandler`) and `Mesh.h`'s thin
  forwards (Task 14) call — cross-checked they match. `E2EKeyLookup.h`'s
  (Task 9) and `PeerEnrollment.h`'s (Task 10) function signatures are the
  ones Tasks 11-13 call — cross-checked.
- **The FrameAuthorizer task (13) deliberately over-invests in test
  coverage relative to its line count** — this is intentional, not
  padding: it's the plan's highest-consequence single function, and Round
  1's Task 6 already demonstrated what happens when a security-adjacent
  decision ships without direct tests (a Critical-severity gap that only
  the final whole-branch review caught, requiring a fix round). Front-loading
  the tests here is cheaper than a repeat of that fix-round cycle.
