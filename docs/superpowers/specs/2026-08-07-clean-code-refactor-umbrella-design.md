# Umbrella Design: Clean-Code Refactor (lattice-nodes)

**Date:** 2026-08-07
**Status:** Approved (umbrella) — per-phase implementation plans generated on pickup
**Scope:** lattice-nodes firmware only, decomposed into 4 sequenced phases (A–D).
**Relationship to prior work:** standalone effort. Not a continuation of the
2026-07-22 close-all-open-issues umbrella (which is closed out as of Phase J +
TLS follow-up, main `d7c27c3`). No shared phase lettering, no dependency on
that document.

## Purpose

Bring lattice-nodes firmware closer to Robert Martin's Clean Code principles —
proper encapsulation, inheritance where it earns its keep, single-responsibility
classes/namespaces — and split files that have grown unmaintainably large.
`Mesh.cpp` (1382 lines, 8-9 distinct jobs) is the known worst case, but no line
count is treated as the threshold — Phase A audits every file for size and
responsibility count, not just the ones already known to be big. Functionality
is preserved
throughout; no wire changes, no behavior changes. Where an existing library
could replace hand-rolled code more efficiently, evaluate it — but see the
library-caution clause below. Docs get rewritten last, once the new shape is
real, since they've been stale since 2026-07-16 (predate Phase 0 through J
entirely).

This document is the decomposition and sequencing spec. It does not contain
task-level implementation detail; each phase gets its own implementation plan
(via the writing-plans skill) when it is picked up.

## Global constraints

- **Firmware-only.** No `lattice-hub` or `lattice-protocol` touches unless
  Phase A's audit surfaces a genuine cross-repo need (not expected).
- **No wire-format changes.**
- **No backwards-compat shims, no data migration** — flag-day only, per the
  project's standing no-backcompat rule. Persisted state may reset on reflash
  where a phase touches it.
- **Encapsulation yes, inheritance sparingly.** Classes for state+behavior
  grouping, private members, clear interfaces — but virtual dispatch /
  inheritance hierarchies only where a real polymorphic need exists (the
  codebase already has one: `Adapter` base with `PirAdapter`/`SerialAdapter`
  derived). No inheritance introduced for its own sake.
- **Library-caution clause.** Phase J (2026-08-06) reverted a libsodium swap
  back to hand-rolled mbedtls wrapping because the "cleaner" library cost 92.5
  KB more flash than the existing code. Any library-replacement candidate this
  effort proposes needs a measured flash/RAM delta before acceptance — a nicer
  API is not sufficient justification on its own.
- **Tiger Style preserved** — static allocation after `setup()`, WDT feeding,
  no new heap churn on hot paths.
- **Full unit + e2e regression required per phase.** CI size-delta reported in
  every PR body — a readability change must not silently regress the flash/RAM
  budget the project has spent multiple phases earning back.

## Phase map

| Phase | Scope | One-line |
|---|---|---|
| A — Audit | nodes | Survey every file under `firmware/main/src` (`mesh/`, `adapter/`, `hardware/`, `app/`, `persistence/`, `error/`, `logging/`, `network/`, `crypto/`) for excessive size, SRP violations, God-objects, missing encapsulation, real inheritance opportunities, library-replacement candidates. No line-count threshold — every file gets sized and judged on responsibility count, not just the ones already known to be big. Ranked findings doc. |
| B — Mesh.cpp decomposition | nodes | Extract collaborator classes out of `Mesh` via composition (transport, beacon, downlink-router are the known candidates); `Mesh` becomes a thin orchestrator. Boundaries finalized from Phase A findings. Dedicated phase because it's the confirmed worst offender (1382 lines, 8-9 jobs) — not because it's assumed to be the only one. |
| C — Repo-wide sweep | nodes | Every other file Phase A flags as excessively large or overloaded (candidates so far: `EepromManager.cpp` 651 lines, `main.cpp` 579 lines — final list from Phase A, not capped to these) plus cross-cutting encapsulation/OOP items. |
| D — Docs rewrite | nodes | README + `docs/*.md` rewritten to describe the post-A/B/C architecture, covering the history through Phase J/TLS-follow-up that current docs predate entirely. |

## Dependency graph & sequencing

```
Phase A (audit)  ──┬── gates Phase B (boundaries come from findings)
                    └── gates Phase C (sweep scope comes from findings)
Phase B ────────────── Mesh.cpp specifically; can start once A's Mesh-relevant
                        findings land, doesn't need to wait for all of A
Phase C ────────────── everything else A finds; independent of B
Phase D ────────────── strictly last — documents the end state, not a moving target
```

- Phase A is a hard prerequisite for B and C (both consume its findings).
- B and C are independent of each other and may run in parallel once A lands.
- D waits on both B and C.

## Per-phase notes

### Phase A — Clean-Code audit

Mirrors the format of the prior effort's `2026-08-04-post-phaseG-audit-findings.md`:
parallel survey agents, each covering a subsystem, findings ranked and written
to a single ledger doc. Axes to survey for, per subsystem:

- **File size / responsibility count** — line-count every file under
  `firmware/main/src` and note how many distinct jobs it does. No arbitrary
  threshold (1000 lines was an illustrative example, not a cutoff) — a
  300-line file doing 4 unrelated things is as much a finding as a 1000-line
  one. `Mesh.cpp` is the known headline case (see Phase B); everything else
  large or overloaded feeds Phase C.
- **Missing encapsulation** — public mutable state, getters/setters standing
  in for real behavior, callers reaching into internals.
- **Real vs. artificial inheritance opportunities** — places where a base
  class + virtual dispatch would remove duplicated logic (like `Adapter`
  already does), vs. places where forcing inheritance would just add a vtable
  for no behavioral gain.
- **Library-replacement candidates** — flagged with an estimated flash/RAM
  delta, not just a suitability note (per the library-caution clause).
- **Architecture-boundary reference** — lattice-hub's package layering
  (`server/orchestrator`, `server/sidecar`, `server/dashboard` as separate,
  cleanly-bounded services) is a useful pattern reference for how nodes'
  subsystems (mesh/adapter/hardware/persistence) should relate to each other.
  This is a pattern reference only — no code reuse, hub is Go and nodes is
  C++.

Output: a ranked findings doc, bucketed into Phase B (Mesh.cpp-specific) and
Phase C (everything else).

### Phase B — Mesh.cpp decomposition

Already directionally scoped by reading the current file. `Mesh` currently
does the following, beyond orchestration:

1. Raw ESP-NOW transport (`setupWiFi`, `setupEspNow`, send/recv callbacks,
   `sendMessage`, `broadcastToAllPeers`, `sendBroadcast`, `drainRecvQueue`)
2. Message building/dispatch (`buildMessage`, `transmitCore`,
   `transmitDispatch`, `transmit*`, `broadcastAdapterData*`,
   `sendDownlinkToNode*`)
3. Master-beacon logic (`broadcastMasterBeacon`, `checkMasterTimeout`,
   `processMasterBeacon`)
4. Downlink routing (`relayDownlink`, `sendRouteReport`,
   `processRouteReport`, `registerDownlinkPeer`)
5. Mesh-key persistence (`loadMeshKeyFromEEPROM`, `saveMeshKeyToEEPROM`)
6. Sequence/replay guarding (`nextSeqGuarded`, `_checkEpochRollback`)

...on top of orchestrating existing collaborators it already delegates to
correctly (`Enrollment`, `PeerRegistry`, `RouteTable`, `NeighborTable`,
`E2EKeyStore`). The direction: extract (1), (3), (4) into their own
collaborator classes held by composition; fold (5) into the existing
`EepromManager` namespace instead of Mesh-local methods; leave (2) and (6) on
`Mesh` itself since they're closer to its actual orchestration job. Exact
class boundaries and naming finalized in the Phase B implementation plan,
informed by Phase A's findings.

### Phase C — Repo-wide sweep

Scope determined by Phase A's findings outside Mesh.cpp. Known candidates
going in: `EepromManager.cpp`, `main.cpp`. Full scope written into the Phase C
implementation plan once Phase A lands.

### Phase D — Docs rewrite

README + `docs/memory_usage.md`, `docs/error_codes.md`,
`docs/server_requirements.md`, `docs/adapter_development_guide.md` rewritten
to reflect the post-A/B/C architecture. Current docs are stale since
2026-07-16 — they predate Phase 0 (ESP-IDF migration) through Phase J (crypto
revert) and the TLS-disable follow-up entirely. Sequenced last so the docs
describe the real end state instead of a moving target.

## Deliverable structure

- **This umbrella spec** — committed now; the sequencing + decision record.
- **Per-phase implementation plans** — generated via the writing-plans skill
  when each phase is picked up.
- **First plan:** Phase A (audit), since it gates B and C.

## Open risks / notes

- **Phase B's exact collaborator boundaries are not locked** — Phase A may
  surface a different natural seam than the (transport / beacon / router)
  split sketched above. That split is a starting hypothesis, not a locked
  decision.
- **Trampolines** (`dataRecvTrampoline`, `registerPeerWithKeyTrampoline`) are
  static functions living in `Mesh`'s class scope because ESP-NOW callbacks
  need a C-compatible function pointer. Extracting transport into its own
  class needs a plan for where these live — likely on the new transport class
  itself, following the same static-trampoline-to-instance-method pattern
  `Mesh` already uses.
- **No upstream GitHub issues drive this effort** — audit-driven, like the
  prior H2/I phases were.
