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
| B — Mesh subsystem cleanup | nodes | Extract collaborator classes out of `Mesh` via composition (transport, beacon, downlink-router are the known candidates); `Mesh` becomes a thin orchestrator. Headlined by `Mesh.cpp` decomposition but scoped to the whole `mesh/` directory — boundaries finalized in the Phase A ledger. Dedicated phase because `Mesh.cpp` is the confirmed worst offender (1382 lines, 8-9 jobs) — not because it's assumed to be the only file touched. |
| C — Repo-wide sweep | nodes | Every other file Phase A flags as excessively large or overloaded (candidates so far: `EepromManager.cpp` 651 lines, `main.cpp` 579 lines — final list from Phase A, not capped to these) plus cross-cutting encapsulation/OOP items. |
| D — Docs rewrite | nodes | README + `docs/*.md` rewritten to describe the post-A/B/C/E architecture, covering the history through Phase J/TLS-follow-up that current docs predate entirely. |
| E — Array-in-interface consistency | nodes | Not Phase A-sourced — discovered via CodeQL's `cpp/array-in-interface` query firing on Phase B's PR (6 new alerts, dismissed as won't-fix pending this phase; 1 pre-existing undismissed alert on `Adapter.cpp:31`; ~9 total pre-existing array-syntax MAC parameter sites repo-wide). Scope: decide whether to adopt `std::array<uint8_t,6>` (or similar) for fixed-size buffer parameters across `mesh/`, `adapter/`, `network/`, `crypto/`, touching every call site of the ~15+ affected functions — not just the declarations. **Must reconcile with Phase A finding 7**, which already deleted a `std::array`-like `MacAddress` value-type wrapper as "strictly worse" than raw-byte handling; this phase needs to either justify why `std::array` parameters are different from that rejected wrapper, or conclude the CodeQL alerts should stay dismissed. |

**Phase list is not closed.** If Phase A surfaces a finding large enough to
warrant its own dedicated implementation plan — same bar `Mesh.cpp` cleared
for Phase B — it gets its own new phase (E, F, ...) instead of being crammed
into Phase C. C is for the sweep of everything else, not a dumping ground for
anything oversized. (Phase E itself is the first example of a phase *not*
sourced from Phase A — new findings can surface from any later phase's CI
results too, not just the original audit.)

## Dependency graph & sequencing

```
Phase A (audit)  ──┬── gates Phase B (boundaries come from findings)
                    └── gates Phase C (sweep scope comes from findings)
Phase B ────────────── the mesh/ subsystem, headlined by Mesh.cpp; can start once
                        A's mesh-relevant findings land, doesn't need to wait for all of A
                        └── surfaced Phase E (CodeQL finding from B's own PR CI, not from A)
Phase C ────────────── everything else A finds; independent of B and E
Phase E ────────────── independent of B/C's own scope; gated only by its CodeQL source
                        existing (i.e. after B merges, since B is what triggered it)
Phase D ────────────── strictly last — documents the end state, waits on B, C, and E
```

- Phase A is a hard prerequisite for B and C (both consume its findings).
- B and C are independent of each other and may run in parallel once A lands.
- E depends only on Phase B having merged (it's scoped from Phase B's CI findings); independent of C.
- D waits on B, C, and E.

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

Output: a ranked findings doc, bucketed into Phase B (mesh/ subsystem, headlined
by Mesh.cpp) and Phase C (everything else) — the exact partition is drawn on
directory rather than file-literal grounds; see the ledger's bucket rule.

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

**Scope is cross-repo, tracked per-repo.** The Lattice ecosystem is 3 repos
(`lattice-nodes`, `lattice-hub`, `lattice-protocol`); each gets its own
docs-rewrite effort, executed independently in its own repo. Each repo's docs
should give an accurate high-level view of the whole stack, but go into deep
detail only for the repo they live in.

- **`lattice-nodes` (this repo) — DONE, merged PR #102.** README.md,
  REFACTORING_GUIDE.md, `docs/memory_usage.md` (real `idf.py build`/`size`
  numbers, not estimates), `docs/error_codes.md`, `docs/server_requirements.md`,
  `docs/adapter_development_guide.md` rewritten; `docs/hardware_requirements.md`
  and `docs/getting_started.md` added (new). All grounded in grep-verified
  current source (post-A/B/C/E architecture) plus a brief, accurate
  `lattice-hub`/`lattice-protocol` ecosystem overview. Old docs were stale
  since 2026-07-16 — predated Phase 0 (ESP-IDF migration) through Phase J
  (crypto revert) and this whole umbrella (A/B/C/E) entirely.
- **`lattice-hub` — NOT STARTED.** Tracked at
  [superbrobenji/lattice-hub#121](https://github.com/superbrobenji/lattice-hub/issues/121).
  Deep hub detail (orchestrator/dashboard/artist-portal/sidecar, serial
  transport, REST API, Kafka pipeline), brief accurate nodes+protocol overview.
- **`lattice-protocol` — NOT STARTED.** Tracked at
  [superbrobenji/lattice-protocol#40](https://github.com/superbrobenji/lattice-protocol/issues/40).
  Deep protocol/codegen detail, brief accurate nodes+hub overview.

**The umbrella-wide "Phase D" is not complete until all 3 land** — this
repo's own Clean-Code refactor umbrella (Phases A-E) is fully done regardless,
since `lattice-hub`/`lattice-protocol`'s docs efforts are separate work items
in separate repos, not blocking this repo's own phase sequence.

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
