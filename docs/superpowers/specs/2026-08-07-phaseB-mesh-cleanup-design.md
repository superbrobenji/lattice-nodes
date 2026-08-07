# Phase B — Mesh subsystem cleanup

**Status:** Approved
**Date:** 2026-08-07
**Repo:** lattice-nodes only. No cross-repo, no wire changes, no behavior changes.
**Parent:** `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (Phase B).
**Findings source:** `docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md`, findings 1, 2, 5, 6, 15, 16, 19.

## Context

Phase A's audit confirmed `Mesh` (1382 lines) does 6 jobs beyond orchestrating
its existing collaborators (`Enrollment`, `PeerRegistry`, `RouteTable`,
`NeighborTable`, `E2EKeyStore`), found a 7th (`processAdapterData`'s
interleaved routing/security logic), and found 3 encapsulation breaks
(`PeerRegistry`, `Enrollment`, `ReplayCache` all expose raw fields that
`Mesh.cpp` reaches into directly instead of going through a method surface).
The ledger's bucket rule is directory-based (`mesh/` → Phase B), which pulled
in `Enrollment`'s and `ReplayCache`'s fixes even though they're not literally
`Mesh.cpp` — doing so keeps this phase and Phase C from editing the same
`Mesh.cpp` call sites in parallel.

## Sequencing

**Encapsulation fixes first, collaborator extraction second.** Once
`Enrollment::learnMasterMac()` exists (Task 2), the beacon collaborator
(Task 6) calls it directly — writing each call site once instead of twice.
The three encapsulation tasks (1-3) are independent of each other and of the
extraction tasks; the three extraction tasks (4-6) depend on the
encapsulation tasks landing first (their new collaborators call the new
private methods, not raw fields).

```
Task 1 (PeerRegistry)  ─┐
Task 2 (Enrollment)    ─┼── independent of each other and of Tasks 4-6's internals;
Task 3 (ReplayCache)   ─┘   land first so Tasks 4-6 write each new call site once
Task 4 (MeshTransport) ── needs Task 1 (uses PeerRegistry's new iteration API in setupEspNow's peer-registration loop)
Task 5 (MasterBeacon)  ── needs Task 2 (calls learnMasterMac/learnSecondaryMasterMac instead of reaching into fields)
Task 6 (DownlinkRouter)── no hard dependency on Tasks 1-3; independent of Tasks 4-5
Task 7 (Mesh becomes thin orchestrator) ── needs Tasks 4-6 done; folds key-persistence into EepromManager calls, wires the 3 new collaborators, removes now-dead code
```

## Design

### Task 1 — `PeerRegistry` encapsulation (finding 5)

**Where:** `PeerRegistry.h:32-33` (`peerMacs`, `peerCount`, both `public`);
reached into at `Mesh.cpp:337-338,478,482-485,1048-1051,1111,1113-1114,1140`
and `Mesh.h:402-403` (`getPeerList()`/`getPeerCount()` leak the raw pointer).

**Fix:** make `peerMacs`/`peerCount` private (same fixed array, zero-alloc,
no behavior change). Add a const iteration surface — `size_t count() const`
(already effectively `getPeerCount()`) plus either `const PeerInfo& at(size_t
i) const` or `begin()`/`end()` over the live prefix. Route both `Mesh`'s
internal loops and `getPeerList()`'s external contract through this instead
of the raw array/count fields.

### Task 2 — `Enrollment` encapsulation + pending-relay queue extraction (findings 6 + 16)

**Where (finding 6):** `Enrollment.h:22-25` (`hasMasterMac`,
`knownMasterMac`, `hasMasterMacSecondary`, `knownMasterMacSecondary`, all
`public`); reached into at `Mesh.cpp:304,494,810-828,840-841,917,1020-1024`.

**Fix:** make the 4 fields `private`. Add `Enrollment::learnMasterMac(const
uint8_t* mac)` / `learnSecondaryMasterMac(const uint8_t* mac)` owning the
memcpy + flag-set + `lattice::eeprom::saveKnownMasterMac(...)` triple that's
currently duplicated at 3 sites in `Mesh.cpp` (817-819, 826-828, 840-841) and
already exists privately at 2 sites in `Enrollment.cpp`
(161-164, 185-188, inside `processJoinAck`) — fold all 5 through the new
methods.

**Where (finding 16):** `Enrollment.h:61-85` — `PendingRelay` struct,
`_pendingRelayQueue`/`_pendingRelayQueueStruct`/`_pendingRelayQueueStorage`
(a static FreeRTOS ring buffer, native since Phase I item OO — the storage
mechanism is not hand-rolled, but the queueing *responsibility* is inline
inside `Enrollment` rather than its own type), `enqueuePendingRelay`,
`drainPendingRelay`, `setPendingRelay`.

**Fix:** extract a small `PendingRelayQueue` type (owns the ring-buffer
handle/struct/storage + `push`/`drain` methods over `PendingRelay`-shaped
records) that `Enrollment` holds as a member, instead of owning the
ring-buffer plumbing directly. `Enrollment::setPendingRelay`/
`drainPendingRelay` become one-line delegations. Same storage, same bound
(`PENDING_RELAY_QUEUE_SIZE = 4`), no behavior change.

**Same files, one pass** — both findings touch `Enrollment.h`/`.cpp`
exclusively (aside from `Mesh.cpp`'s call-site updates for finding 6).

### Task 3 — `ReplayCache` split (finding 15)

**Where:** `ReplayCache.h:13-32` — a bare `struct` bundling (a) the actual
per-origin replay-detection table (`cache[]`, `isReplay()` — correctly
encapsulated already, no external access to `cache[]`) with (b) this node's
own outbound bookkeeping: `bootEpoch`, `txSeqNum`/`nextSeq()`,
`lastRelayedEpoch`, `lastRelayedSeqNum` — all public, mutated directly by
`Mesh.cpp` (`replay.bootEpoch = epoch;` at line 206;
`replay.lastRelayedEpoch = msg.epoch_num; replay.lastRelayedSeqNum =
msg.seq_num;` at 892-893).

**Fix:** keep `ReplayCache` narrowly scoped to incoming-message replay
detection (`cache[]`/`isReplay()` only). Move `bootEpoch`/`txSeqNum`/
`lastRelayedEpoch`/`lastRelayedSeqNum` — this node's own outbound sequence/
replay-guarding state (job 6 from finding 1, which stays on `Mesh`) — onto
`Mesh` directly, or a small dedicated struct with real mutator methods
(`bumpEpoch()`, `markRelayed(epoch, seq)`, `wasRelayedBefore(epoch, seq)`)
instead of raw public fields. Prefer the dedicated-struct form if `Mesh.h`
is already getting new member fields from Tasks 4-6 — keeps the new state
grouped rather than adding 4 more loose fields to `Mesh` itself.

### Task 4 — Extract `MeshTransport` (finding 1, job 1; finding 19)

**Where:** `Mesh.cpp` — `setupWiFi:279`, `setupEspNow:312`,
`onDataSentCallback:347`, `onDataRecvCallback` (`Mesh.h:73`),
`dataRecvTrampoline:453`, `sendMessage:460`, `broadcastToAllPeers:477`,
`sendBroadcast:733`, `drainRecvQueue:390`. Plus `MeshCrypto.h:16`
(`registerPeerWithEspNow`) — opportunistic per finding 19, moves here since
it's peering, not crypto.

**Fix:** new `MeshTransport` class owning ESP-NOW radio setup, the RX
callback + trampoline + lock-free SPSC ring (keep as native FreeRTOS ring
buffer — Phase I item OO, don't replace), and outbound send primitives.
`Mesh` holds one as a member, calls into it for all radio I/O. The RX
trampoline pattern (`static` function + instance dispatch via a stored
`this`-equivalent pointer) moves with the callback it serves — same pattern
`Mesh` already uses for `dataRecvTrampoline`/`registerPeerWithKeyTrampoline`,
just relocated.

**Note on `processAdapterData`'s routing-decision block:** finding 2's
routing-decision half (lines 919-956 of the current `Mesh.cpp` —
relay-toward-master, forward-on-route, flood-fallback; no crypto involved)
moves to `DownlinkRouter` (Task 6), not here — it uses `registerDownlinkPeer`
and `relayDownlink`, which are downlink-router responsibilities, even though
it also calls `transmitCore`/`sendMessage` (transport primitives it reaches
via `Mesh`, same as `DownlinkRouter`'s other methods will).

### Task 5 — Extract `MasterBeacon` (finding 1, job 3)

**Where:** `Mesh.cpp` — `broadcastMasterBeacon:603`, `checkMasterTimeout:763`,
`processMasterBeacon:781`.

**Fix:** new `MasterBeacon` class owning master-role beacon broadcast, master-
timeout detection, and incoming-beacon processing (distance/freshness
tracking via `NeighborTable`, TOFU master-MAC learning via `Enrollment`'s new
`learnMasterMac`/`learnSecondaryMasterMac` from Task 2). `Mesh` holds one as
a member.

### Task 6 — Extract `DownlinkRouter` (finding 1, job 4 — narrowed; finding 2's routing half)

**Scope narrowed during implementation planning.** The design's first pass
(this section, originally) put `sendRouteReport`/`processRouteReport` in
scope alongside `relayDownlink`/`registerDownlinkPeer`, following finding
1's job-4 grouping literally. Reading both functions in full for the
implementation plan showed they're not routing in the same sense —
`processRouteReport`'s master branch does E2E open (`peerE2EKeys`,
`openPayload`), reconstructs and verifies a per-hop chain-MAC
(`routemac::buildHopContext`/`chainStep` against `msg.auth_path`), and only
*then* touches `RouteTable::record`; `sendRouteReport` seals via
`transmitCore`. Both are exactly the kind of security-critical, heavily
cross-referenced (issue #44, chain-MAC) crypto+routing hybrid that
`processAdapterData`'s security half already established shouldn't be split
casually. **`sendRouteReport` and `processRouteReport` stay on `Mesh`** —
same reasoning as `processAdapterData`'s security sequence, not a new
exception.

**Where:** `Mesh.cpp` — `relayDownlink:1042`, `registerDownlinkPeer:140`,
plus `processAdapterData`'s routing-decision block (lines 919-956):
relay-toward-master when addressed to master and not self/broadcast;
source-route forwarding when the frame carries a route path we're on; flood
fallback via `relayDownlink` otherwise.

**Fix:** new `DownlinkRouter` class owning downlink relay,
auto-peer-registration for forwarding, and the routing *decision* (not the
security/E2E half — see below). `Mesh::processAdapterData` keeps its current
shape for lines 958-1039 (security gate → E2E open → config-opcode
authorization → local delivery — **do not split this further**, it's one
atomic, heavily-commented security check sequence with specific attack
scenarios documented inline) and delegates the routing decision (currently
lines 919-956) to `DownlinkRouter` with an early return, e.g.:

**Concrete design** (resolved during implementation planning, replaces the
earlier placeholder `tryRouteAway` sketch): `DownlinkRouter` stays crypto-free
— it *classifies* what should happen to a frame; `Mesh` executes the
crypto-touching action itself, since `transmitCore` (used by the
relay-toward-master case) needs `masterE2EKeys`/`_checkEpochRollback`, which
live on `Mesh`. This mirrors how `NeighborTable`/`RouteTable`/`E2EKeyStore`
already work in this codebase — passive data+logic classes `Mesh` calls into
with whatever external state they need as parameters, never holding a
back-reference to `Mesh` itself.

**Post-implementation correction (Task 6):** the sketch below originally
shipped with a 3-value `RouteDecision` (`NotRouted`, `RelayTowardMaster`,
`ForwardOnRoute`, `Flood` — no hop-limit case), with the per-case
`if (msg.hop_count >= MAX_HOPS) return;` check left inline in
`processAdapterData`'s switch cases. That's wrong: the original code's
hop-limit check is an unconditional early-return from the *whole*
`processAdapterData` function (drop the frame outright, never reach the
security gate below), not a per-case concern local to the switch. A 4th/5th
distinction was needed because `NotRouted` is defined to *fall through* to
the security gate — collapsing the hop-limit case into it would have let a
hop-limit-exceeded frame wrongly reach local delivery. `RouteDecision` grew a
5th value, `DropHopLimitExceeded`, and `classify()` itself now performs the
hop-limit check (it has `msg` in scope already) rather than leaving it to the
caller. The blocks below reflect what actually shipped, including the
`DownlinkRouter router;` member name (`Mesh.h`) and the real 3-argument
`MeshTransport::sendMessage(target, msg, deviceMac)` signature Task 4
concretized after this sketch was first drafted.

```cpp
// DownlinkRouter.h
enum class RouteDecision { NotRouted, RelayTowardMaster, ForwardOnRoute, Flood,
                           DropHopLimitExceeded };

class DownlinkRouter {
public:
  // Read-only classification — no state mutation, no I/O. nextHopMacOut is
  // written only when the result is ForwardOnRoute. DropHopLimitExceeded is
  // NOT the same as NotRouted: it means this WAS a routing case (addressed
  // to master, or on the frame's source route) but hop_count is already at
  // MAX_HOPS, so the caller must drop the frame outright rather than fall
  // through to the security gate (see NotRouted's fall-through contract).
  RouteDecision classify(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                         bool addressedToSelf, bool isBroadcastTarget, bool addressedToMaster,
                         uint8_t nextHopMacOut[6]) const;

  void relayDownlink(const mesh_message& msg, const PeerRegistry& peers, const uint8_t* deviceMac,
                     MeshTransport& transport);
  void registerDownlinkPeer(const uint8_t* mac, const PeerRegistry& peers,
                            const MasterInfo& currentMaster);

private:
  uint8_t downlinkPeerLru[lattice::config::LATTICE_DOWNLINK_PEER_MAX][6]{};
  size_t downlinkPeerLruCount{0};
};
```

```cpp
// Mesh::processAdapterData's routing block, replacing lines 919-956
uint8_t nextHop[6];
switch (router.classify(msg, deviceMacAddress, isMaster, addressedToSelf,
                        isBroadcastTarget, addressedToMaster, nextHop)) {
case RouteDecision::DropHopLimitExceeded:
  return; // drop the whole frame — matches the original's unconditional return
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
  transport.sendMessage(nextHop, fwd, deviceMacAddress);
  return;
}
case RouteDecision::Flood:
  router.relayDownlink(msg, peers, deviceMacAddress, transport);
  return;
case RouteDecision::NotRouted:
  break; // fall through to the security gate / local delivery, unchanged
}
```

### Task 7 — `Mesh` becomes thin orchestrator (finding 1, jobs 2 + 5 + 6)

**Where:** `Mesh.h`/`Mesh.cpp` — after Tasks 1-6, wire the 3 new
collaborators as members, remove the now-moved methods, and fold
`loadMeshKeyFromEEPROM`/`saveMeshKeyToEEPROM` (job 5) into direct
`lattice::eeprom::loadMeshKey`/`saveMeshKey` calls (already exist per the
architecture-boundary reference — no new `EepromManager` work needed, just
removing `Mesh`'s wrapper methods if they're pure pass-throughs; keep them if
they add real logic beyond the call).

**What stays on `Mesh` itself:** message building/dispatch (job 2:
`buildMessage`, `transmitCore`, `transmitDispatch`, `transmit*`,
`broadcastAdapterData*`, `sendDownlinkToNode*`), sequence/replay guarding
(job 6: `nextSeqGuarded`, `_checkEpochRollback`, plus Task 3's relocated
outbound-state struct), `processAdapterData`'s security half, and
orchestration (`init`, `loop`, wiring the 3 collaborators + existing
`Enrollment`/`PeerRegistry`/`RouteTable`/`NeighborTable`/`E2EKeyStore`
together).

## Non-goals

- No wire changes.
- No hub/protocol touches.
- No behavior changes anywhere — this is a pure structural refactor. Every
  check, every log line, every timing behavior stays identical; only which
  class owns the code changes.
- Findings 3, 4, 17 (Phase C) not touched here even though they're large —
  bucketed to Phase C by the ledger, stay there.
- Finding 7 (`MacAddress` dead-code deletion) and finding 14
  (`housekeeping_task_fn`'s enrollment-broadcast extraction, lands as a new
  `Mesh` method) are Phase C's — Phase B should expect finding 14's new
  method arriving in `Mesh` as an addition, which doesn't conflict with any
  extraction here.

## Testing

- Full unit + e2e regression after every task — this phase changes internal
  structure only, so the existing 257-test host suite should stay green
  throughout with zero test-content changes, only (where a test reaches into
  an internal it's relocating) updated call sites.
- No new tests required by this refactor itself (behavior is unchanged), but
  if any task's extraction reveals an untested branch, add coverage for it.
- CI size delta reported per PR per the umbrella spec's global constraint —
  expect near-zero (composition instead of free functions/methods on `Mesh`
  doesn't add vtables; no new heap allocation).

## Files touched (estimate)

- `PeerRegistry.h`: private fields + iteration API (Task 1).
- `Enrollment.h`/`.cpp`: private TOFU fields + `learnMasterMac`/
  `learnSecondaryMasterMac`; new `PendingRelayQueue` type (Task 2).
- `ReplayCache.h`: narrowed to `cache[]`/`isReplay()`; new outbound-state
  struct/methods, likely landing in `Mesh.h` or a new small header (Task 3).
- New: `MeshTransport.{h,cpp}` (Task 4), `MasterBeacon.{h,cpp}` (Task 5),
  `DownlinkRouter.{h,cpp}` (Task 6).
- `Mesh.{h,cpp}`: shrinks substantially across Tasks 4-7 as methods move out;
  final pass (Task 7) wires the 3 collaborators and removes now-dead code.
- `MeshCrypto.h`: loses `registerPeerWithEspNow` (moves to `MeshTransport`,
  Task 4); keeps `generateKeypair`.

## Est. impact

Primary goal is maintainability, not size — `Mesh.cpp` from 1382 lines to an
orchestrator of a few hundred; 3 new focused collaborator classes each
independently readable/testable. Expect near-zero flash/RAM delta (no new
virtual dispatch, no new heap allocation, same total code moved not added).
