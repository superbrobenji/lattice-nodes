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

## Round 2 (Tasks 8-14) — Mesh was still 1020 lines after Tasks 1-7

Tasks 1-7 shipped, reviewed clean, CI green (272/272 unit + 41/41 e2e,
ESP-IDF build passing). `Mesh.cpp` dropped from 1382 to 1020 lines — this
document's original "Est. impact" section said "an orchestrator of a few
hundred [lines]"; that estimate was wrong. It didn't account for how much
was explicitly scoped to *stay* on `Mesh`: message dispatch (job 2, ~190
lines), sequence/replay guarding (job 6, ~35 lines), `processAdapterData`'s
security half (~136 lines, protected by an explicit design decision),
`sendRouteReport`/`processRouteReport` (~134 lines, Task 6's mid-flight
scope-narrowing for the same security reason), plus a ~154-line
peer-enrollment-coordination cluster that was never assigned to move
anywhere, and a ~46-line uplink-routing helper. That's ~700 of the
remaining 1020 lines accounted for by decisions that were each individually
reasoned, but never added up and checked against a real target.

Round 2 finishes the job, genuinely applying single-responsibility to
everything still on `Mesh`, **including** the previously-protected security
code — with the same discipline `DownlinkRouter::classify()` already
proved out: separate the *decision* from the *execution*, don't touch the
security logic's actual checks, ordering, or comments while relocating it.

Target end state for `Mesh`: `init`/`setupRadio`/`loadPersistentState`/
`loop` (boot + main-loop orchestration), `handleReceivedMessage`'s dispatch
switch, and thin delegation into the classes below. Realistic estimate this
time, cross-checked against actual current line counts per class:
**~250-350 lines** — this is the number Round 1's estimate should have been.

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

**Round 2 (Tasks 8-14), all sequential — each narrows what's left in
`Mesh.cpp` before the next task starts, same reasoning as Tasks 4-7:**

```
Task 8  (OutboundSequenceState grows) ── self-contained, needed by Tasks 11/12/13's sends
Task 9  (E2EKeyLookup.h, free functions) ── self-contained, needed by Tasks 11/12/13
Task 10 (PeerEnrollment.h + UplinkRouter) ── independent of 8/9; needed by Task 11 (relayEnrollmentUplink moves through MeshMessenger, which needs UplinkRouter for findNextHopToMaster)
Task 11 (MeshMessenger) ── needs Tasks 8, 9, 10 (nextSeqGuarded, E2E lookups, uplink routing)
Task 12 (RouteReportHandler) ── needs Tasks 8, 9, 11 (sendRouteReport calls transmitCore-equivalent)
Task 13 (FrameAuthorizer) ── needs Task 9 (E2E lookups for the open step); independent of 11/12
Task 14 (Mesh becomes thin orchestrator, round 2) ── needs Tasks 8-13 done
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

### Task 8 — `OutboundSequenceState` grows (`nextSeqGuarded`, `_checkEpochRollback`)

**Where:** `Mesh.cpp` — `nextSeqGuarded` (draws a tx sequence number, bumping
the boot epoch and persisting it on 0xFFFF→0 wrap), `_checkEpochRollback`
(seal-time AEAD nonce-reuse guard, halts the node via `err::fail` on
rollback).

**Fix:** move both into `OutboundSequenceState` (`ReplayCache.h`) as their
own methods — the struct already owns `bootEpoch`/`txSeqNum`, this just
gives it the behavior that was artificially split onto `Mesh`:

```cpp
// ReplayCache.h, added to OutboundSequenceState
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
  if (_lastSealedEpoch == UINT32_MAX) { _lastSealedEpoch = epoch; _lastSealedSeq = seq; return; }
  if (epoch > _lastSealedEpoch) { _lastSealedEpoch = epoch; _lastSealedSeq = seq; return; }
  if (epoch == _lastSealedEpoch && seq > _lastSealedSeq) { _lastSealedSeq = seq; return; }
  lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO, lattice::core::ModuleDigit::MESH, 1,
                     "AEAD epoch rollback — refusing seal");
}
```

`_lastSealedEpoch`/`_lastSealedSeq` (currently `Mesh` members, defaulted
`UINT32_MAX`/`0`) move to `OutboundSequenceState` alongside them.
`ReplayCache.h` needs new includes: `EepromManager.h` (for `saveBootEpoch`),
`error/Error.h` + `error/ErrorCore.h` (for `err::fail`). All call sites
(`Mesh::buildMessage`, `Mesh::transmitCore`'s seal guard, `Mesh::init`'s
epoch bump, `Enrollment::sendRequest`'s caller, `Mesh::enrollPeer`'s ACK
sequencing, `Mesh::sendDownlinkToNode`'s seal guard) become
`txState.nextSeqGuarded()` / `txState.checkEpochRollback(...)`.

### Task 9 — `E2EKeyLookup.h` (free functions, new file)

**Where:** `Mesh.cpp` — `masterE2EKeys`, `peerE2EKeys` (~20 lines
combined).

**Fix:** these bridge 4 collaborators (`MasterInfo`, `PeerRegistry`,
`Enrollment`, `E2EKeyStore`) with no state of their own — free functions,
matching this codebase's existing convention for cross-cutting utilities
(`network/mac_table.h`, `network/MacEq.h`, `network/mem.h`,
`network/hw_mac.h` are all free-function-only headers already). Adding
these as *methods* on any one of the 4 collaborators would give that
collaborator an artificial dependency on the other 3 — exactly the kind of
coupling `E2EKeyStore`'s current design deliberately avoids (it doesn't
know `PeerRegistry` or `Enrollment` exist).

```cpp
// mesh/E2EKeyLookup.h
namespace lattice { namespace mesh {

inline bool masterE2EKeys(const MasterInfo& currentMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys,
                          const uint8_t** kUp, const uint8_t** kDown) {
  if (!enrollment.hasKnownMaster())
    return false;
  PeerInfo* master = peers.find(currentMaster.mac);
  if (!master)
    return false;
  return e2eKeys.getKeys(master->mac, enrollment.getPrivateKey(), master->publicKey, kUp, kDown);
}

inline bool peerE2EKeys(const uint8_t* originMac, PeerRegistry& peers, Enrollment& enrollment,
                        E2EKeyStore& e2eKeys, const uint8_t** kUp, const uint8_t** kDown) {
  PeerInfo* peer = peers.find(originMac);
  if (!peer)
    return false;
  return e2eKeys.getKeys(peer->mac, enrollment.getPrivateKey(), peer->publicKey, kUp, kDown);
}

} } // namespace lattice::mesh
```

Bodies are verbatim from the current `Mesh::masterE2EKeys`/`peerE2EKeys` —
only the collaborator access changes from implicit member access to
explicit parameters. Every call site (`Mesh::transmitCore`,
`Mesh::sendDownlinkToNode`, and Tasks 11-13's new classes) calls
`lattice::mesh::masterE2EKeys(currentMaster, peers, enrollment, e2eKeys,
&kUp, &kDown)` instead of `masterE2EKeys(&kUp, &kDown)`.

### Task 10 — `PeerEnrollment.h`/`.cpp` (free functions) + `UplinkRouter` (new class)

**Where:** `Mesh.cpp` — `processJoinAck` (outer relay-vs-delegate
dispatch, ~30 lines), `registerPeerWithKeyTrampoline`/`registerPeerWithKey`
(~40 lines), `addPeer` (~8 lines) → `PeerEnrollment.h`/`.cpp`.
`findNextHopToMaster` (~46 lines) plus the `forwardingPeer[6]` field →
`UplinkRouter` (new class, mirrors `DownlinkRouter`'s shape).

**Fix (`PeerEnrollment.h`/`.cpp`):** same reasoning as Task 9 — peer
approval bridges `PeerRegistry` + `Enrollment` + `MeshTransport`, free
functions:

```cpp
// mesh/PeerEnrollment.h
namespace lattice { namespace mesh {

// Add/rekey a peer (bridges PeerRegistry + Enrollment + MeshTransport::registerPeerWithEspNow).
// allowRekey=false (over-the-air JOIN_ACK) never replaces an established key;
// allowRekey=true (server-approved enrollment) may.
bool registerPeerWithKey(const uint8_t* mac, const uint8_t* publicKey32, bool allowRekey,
                         PeerRegistry& peers, Enrollment& enrollment, bool dualMasterMode);

// Optional UI/app-triggered peer add.
void addPeer(const uint8_t* mac, PeerRegistry& peers);

// Outer JOIN_ACK dispatch: relay (via MeshTransport::sendBroadcast, static) if not addressed to
// this device; else delegate to enrollment.processJoinAck(...). registerFn is the
// RegisterPeerFn trampoline Enrollment's callback signature requires (plain function pointer —
// see Enrollment.h's existing RegisterPeerFn comment on why it can't be a capturing lambda).
void dispatchJoinAck(const mesh_message& msg, const uint8_t* deviceMac, bool isMaster,
                     Enrollment& enrollment, RegisterPeerFn registerFn);

} } // namespace lattice::mesh
```

Bodies move verbatim from `Mesh.cpp`'s current implementations (already
read in full during Round 1 planning — same functions, just relocated).
`registerPeerWithKeyTrampoline`'s job (adapting `registerPeerWithKey` to
`RegisterPeerFn`'s plain-function-pointer signature for
`Enrollment::processJoinAck`) still needs a static trampoline through
`Mesh::instance`, same pattern as `MeshTransport`'s trampolines — this one
stays a 3-line static method on `Mesh` (`registerPeerWithKeyTrampoline`),
since `PeerEnrollment.h`'s functions aren't instance-bound and don't have
anywhere to hang a static callback off of. `Mesh::enrollPeer` (both
overloads) is **not** part of this task — it builds and sends a JOIN_ACK
message, which is message construction/dispatch, not peer-registry
bookkeeping; it moves in Task 11.

**Fix (`UplinkRouter`):** owns the uplink mirror of `DownlinkRouter`'s
forwarding-peer LRU — here a single-slot "LRU" (a node only ever forwards
uplink to one next hop at a time), plus the `NeighborTable`-driven next-hop
selection:

```cpp
// mesh/UplinkRouter.h
class UplinkRouter {
public:
  // Returns the next hop's PeerInfo* (a scratch entry, not a `peers` member — mirrors the
  // current nextHopScratch pattern) or nullptr if no route. Registers the chosen hop with
  // MeshTransport::registerPeerWithEspNow (static) and evicts the prior forwardingPeer from
  // ESP-NOW if it changed and isn't itself an enrolled peer or the current master.
  PeerInfo* findNextHopToMaster(const MasterInfo& currentMaster, PeerRegistry& peers,
                                NeighborTable& neighbors, const uint8_t* deviceMac,
                                uint64_t nowMs);

private:
  uint8_t forwardingPeer[6]{};
  PeerInfo nextHopScratch{};
};
```

Body moves verbatim from `Mesh::findNextHopToMaster` (`Mesh.cpp:84-130`,
already read in full during Round 1 planning) — only the parameter
threading changes (`currentMaster`/`peers`/`neighbors`/`deviceMac`/`nowMs`
passed in instead of implicit member access), and
`lattice::mesh::crypto::registerPeerWithEspNow` becomes
`MeshTransport::registerPeerWithEspNow` (static, per Task 4). `Mesh` holds
one as a member (`UplinkRouter uplinkRouter;`), called from Task 11's
`MeshMessenger::transmitCore` equivalent and from `relayEnrollmentUplink`.

### Task 11 — `MeshMessenger` (new class)

**Where:** `Mesh.cpp` — `buildMessage`, `transmitCore`, `transmitDispatch`,
`transmit`, `transmitSelfOriginated`, `broadcastAdapterData`,
`broadcastAdapterDataStatic`, `sendDownlinkToNode`,
`sendDownlinkToNodeStatic`, `enrollPeer` (both overloads),
`relayEnrollmentUplink` — everything that constructs and dispatches an
outbound message (~260 lines combined, the largest single Round 2
extraction).

**Fix:** new `MeshMessenger` class owning message construction and
dispatch. Depends on `OutboundSequenceState` (Task 8, for sequencing),
`E2EKeyLookup.h` (Task 9, for E2E seal), `UplinkRouter` (Task 10, for
uplink routing), `MeshTransport` (for actual sends), `RouteTable` (for
`sendDownlinkToNode`'s source routing). This is the class with the most
collaborator dependencies in the whole plan — by design: it's the single
place "how does this node send something" lives, so everything that needs
to be threaded through a send naturally converges here.

Static entry points (`transmit`/`transmitSelfOriginated`, called by
`Adapter` via a plain function-pointer member — see `Mesh.h`'s existing
comment on `Adapter::TransmitPtr`) keep routing through `Mesh::instance`
exactly like `MeshTransport`'s and `PeerEnrollment`'s trampolines do — no
new singleton needed, `Mesh` already has one.

All 10 function bodies move verbatim (all read in full during Round 1
planning — `Mesh.cpp:146-167` `buildMessage`, `313-402` transmit family,
`449-529` broadcast/downlink family, `795-847` `enrollPeer`, `693-714`
`relayEnrollmentUplink`); only collaborator access changes from implicit
member access to explicit parameters/constructor injection, and internal
calls to `nextSeqGuarded()`/`masterE2EKeys()`/`peerE2EKeys()`/
`findNextHopToMaster()` become `txState.nextSeqGuarded()`/
`lattice::mesh::masterE2EKeys(...)`/`lattice::mesh::peerE2EKeys(...)`/
`uplinkRouter.findNextHopToMaster(...)`.

### Task 12 — `RouteReportHandler` (new class, security-sensitive)

**Where:** `Mesh.cpp` — `sendRouteReport`, `processRouteReport` (~134
lines, Task 6's Round 1 scope-narrowing note applies here too — chain-MAC
verification via `RouteMac.h`, E2E open/seal, `RouteTable` recording, all
interleaved).

**Fix:** new `RouteReportHandler` class. **Move verbatim — do not
restructure the chain-MAC verify loop, the E2E open sequence, or the
route_len bounds checks while relocating.** Same discipline as
`processAdapterData`'s protected security half and `DownlinkRouter`'s
hop-limit distinction: this function's checks close specific,
comment-documented attack scenarios (issue #44's route-path forgery
class) — get the relocation byte-for-byte right, verified the same way
Round 1's reviewers verified `MasterBeacon::process` and
`DownlinkRouter::classify()` (extract original body from the diff,
normalize only the collaborator-access renames, confirm exact match).

```cpp
// mesh/RouteReportHandler.h
class RouteReportHandler {
public:
  // Returns false if not master and no route to master (nothing sent) — matches
  // Mesh::sendRouteReport's current bool return.
  bool sendRouteReport(bool isMaster, UplinkRouter& uplinkRouter, const MasterInfo& currentMaster,
                       PeerRegistry& peers, NeighborTable& neighbors, const uint8_t* deviceMac,
                       uint64_t nowMs, MeshMessenger& messenger);
  void processRouteReport(const mesh_message& msg, bool isMaster, PeerRegistry& peers,
                          Enrollment& enrollment, E2EKeyStore& e2eKeys, RouteTable* routes,
                          const uint8_t* deviceMac, MeshMessenger& messenger,
                          ExternalRecvCallback externalRecvCallback);
};
```

Depends on Task 11 (`MeshMessenger`, for the relay/transmit calls both
functions make) and Task 9 (`E2EKeyLookup.h`). `routes` stays a raw
pointer parameter (mirrors `Mesh`'s existing `std::unique_ptr<RouteTable>`
ownership — `RouteReportHandler` doesn't own it, `Mesh` does, passes it
through). `ExternalRecvCallback` is `Mesh`'s existing
`std::function<void(const mesh_message&)>` typedef, passed through for the
terminal-delivery call.

### Task 13 — `FrameAuthorizer` (new class, most security-sensitive)

**Where:** `Mesh.cpp` — `processAdapterData`'s security half (lines
958-1039 as originally numbered in Round 1's design, now shifted but
unchanged in shape): master-gate check, E2E open (both directions),
config-opcode authorization. **This is the block Round 1 explicitly
protected** ("do not split this further... one atomic, heavily-commented
security check sequence"). Round 2 revisits that call because leaving it
on `Mesh` is what's blocking a genuine SRP outcome — but the *method* of
extraction is unchanged from how `DownlinkRouter::classify()` handled the
routing half: separate the decision from the execution, preserve every
check/comparison/comment, verify byte-for-byte.

**Fix:** `FrameAuthorizer` owns the authorization *decision and the E2E
open* — it returns either "authorized, here's the opened message" or
"rejected" — because the E2E-open step is itself part of establishing
trust (an unopened, still-sealed frame isn't yet known-authentic), not
separable from the authorization question the way `DownlinkRouter`'s
crypto-free routing decision was. `Mesh` keeps only local delivery
dispatch (the `externalRecvCallback(opened)` call and the
broadcast-re-relay-after-delivery line) — thin, no security logic of its
own.

```cpp
// mesh/FrameAuthorizer.h
enum class AuthResult { Rejected, Authorized };

class FrameAuthorizer {
public:
  // opened is written only when the result is Authorized. Mirrors processAdapterData's
  // current security half exactly: master-not-self-addressed sealed-type gate, E2E open
  // (master unseals uplink / node unseals downlink), config-opcode authorization gate.
  AuthResult authorize(const mesh_message& msg, bool isMaster, bool addressedToSelf,
                      PeerRegistry& peers, Enrollment& enrollment, E2EKeyStore& e2eKeys,
                      mesh_message& openedOut);
};
```

**This task gets dedicated test coverage beyond "run the existing suite"**
— Task 6's fix-round lesson (`DropHopLimitExceeded` shipping with zero
direct tests until the final review caught it) applies with even more
force here, since this is the security gate itself, not routing logic
adjacent to it. The implementation plan must include direct
`FrameAuthorizer::authorize` tests for: sealed-type-not-addressed-to-self
at master (rejected), E2E-open failure both directions (rejected), config
opcode via unopened/broadcast path (rejected — this is the specific
forged-broadcast attack `processAdapterData`'s comments document), config
opcode from non-master origin (rejected), and the legitimate-authorized
path both directions (accepted, `openedOut` populated correctly) — not
inferred from `processAdapterData`-level integration tests alone.

### Task 14 — `Mesh` becomes thin orchestrator, round 2

**Where:** `Mesh.h`/`Mesh.cpp` — after Tasks 8-13, wire the 4 new
collaborators (`UplinkRouter`, `MeshMessenger`, `RouteReportHandler`,
`FrameAuthorizer`) as members, remove now-moved methods/fields
(`forwardingPeer` moves with Task 10; `_lastSealedEpoch`/`_lastSealedSeq`
move with Task 8), rewrite `processAdapterData` to call
`frameAuthorizer.authorize(...)` and dispatch on the result, final
dead-code sweep (same style as Round 1's Task 7), full regression, PR.

**What stays on `Mesh` itself after Round 2:** `init`, `setupRadio`,
`loadPersistentState`, `loop`, `handleReceivedMessage`'s dispatch switch
(routes each message type to the right collaborator/handler),
`debugDumpRadio`, `isSealedType`, the constructor, and thin wiring/glue.
Genuinely an orchestrator — no remaining cluster of unrelated logic.

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
  structure only, so the existing test suite should stay green throughout
  with zero test-content changes, only (where a test reaches into an
  internal it's relocating) updated call sites.
- No new tests required by most tasks in this refactor (behavior is
  unchanged), but if any task's extraction reveals an untested branch, add
  coverage for it. **Exception: Task 13 (`FrameAuthorizer`) requires direct
  dedicated tests of the authorization decision** — see Task 13's design
  note; this is the lesson from Task 6's `DropHopLimitExceeded` fix-round,
  applied proactively this time instead of caught after the fact.
- CI size delta reported per PR per the umbrella spec's global constraint —
  expect near-zero (composition instead of free functions/methods on `Mesh`
  doesn't add vtables; no new heap allocation). Round 2's new free-function
  headers (`E2EKeyLookup.h`, `PeerEnrollment.h`) are `inline`, matching
  `mac_table.h`/`MacEq.h`'s existing pattern — no new translation-unit-level
  code duplication risk beyond what those headers already accept.

## Files touched (estimate)

**Round 1 (Tasks 1-7, shipped):**
- `PeerRegistry.h`: private fields + iteration API (Task 1).
- `Enrollment.h`/`.cpp`: private TOFU fields + `learnMasterMac`/
  `learnSecondaryMasterMac`; new `PendingRelayQueue` type (Task 2).
- `ReplayCache.h`: narrowed to `cache[]`/`isReplay()`; new
  `OutboundSequenceState` struct (Task 3).
- New: `MeshTransport.{h,cpp}` (Task 4), `MasterBeacon.{h,cpp}` (Task 5),
  `DownlinkRouter.{h,cpp}` (Task 6).
- `MeshCrypto.h`: lost `registerPeerWithEspNow` (moved to `MeshTransport`,
  Task 4); kept `generateKeypair`.

**Round 2 (Tasks 8-14):**
- `ReplayCache.h`: `OutboundSequenceState` grows `nextSeqGuarded`/
  `checkEpochRollback` (Task 8).
- New: `E2EKeyLookup.h` (Task 9), `PeerEnrollment.{h,cpp}` +
  `UplinkRouter.{h,cpp}` (Task 10), `MeshMessenger.{h,cpp}` (Task 11),
  `RouteReportHandler.{h,cpp}` (Task 12), `FrameAuthorizer.{h,cpp}` (Task
  13).
- `Mesh.{h,cpp}`: shrinks from 1020 to an estimated ~250-350 lines across
  Tasks 8-14; final pass (Task 14) wires the 4 new collaborators.

## Est. impact

Primary goal is maintainability, not size. **Round 1 alone: 1382 → 1020
lines** (this document's original estimate of "a few hundred" was wrong —
see the Round 2 context note above). **Round 1 + Round 2 combined,
estimated: 1382 → ~250-350 lines**, 7 new focused collaborator
classes/headers total (3 from Round 1, 4 from Round 2), each independently
readable/testable. Expect near-zero flash/RAM delta throughout (no new
virtual dispatch, no new heap allocation, same total code moved not
added) — Round 2's free-function headers are `inline`, same flash profile
as calling the equivalent code inline today.
