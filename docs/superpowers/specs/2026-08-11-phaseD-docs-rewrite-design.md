# Phase D — Docs Rewrite Design

**Status:** Approved, ready for writing-plans.
**Date:** 2026-08-11
**Parent:** `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (Phase D)
**Sequencing:** Last phase of the umbrella — Phases A/B/C/E are merged, so the codebase this phase
documents is the real end state, not a moving target.

## Problem

All 6 existing docs are severely stale — not incrementally out of date, but describing a
different codebase: deleted files (`EEPROM_Manager.h`, `MacAddress.h`), a deleted API (the legacy
`Error::fail` overload Phase C's finding 8 removed), a build system the project stopped using
before this session's tracked work even began (`arduino-cli`/`main.ino`, when the whole tree has
been pure ESP-IDF since Phase 0/I), an architecture missing ~15 collaborator classes Phase B
extracted from `Mesh`, and a wire-format schema missing years of protocol additions (E2E AEAD,
chain-MAC route auth, dual-master, compact message format). `docs/memory_usage.md`'s own premise
("re-measurement blocked, no working build") is itself stale — a real ESP-IDF v5.5.1 toolchain is
available locally (confirmed: `~/esp/esp-idf`, `idf.py --version` succeeds after sourcing
`export.sh`), so this phase can get fresh, real numbers instead of writing more estimated prose.

User also requested two additions beyond the umbrella spec's original "rewrite the stale docs"
scope: hardware requirements + wiring schematics, and an extremely thorough non-technical
build-from-source flashing/configuration walkthrough.

## Scope — 8 documents (6 rewrites + 2 new)

| Doc | Action | Key content |
|---|---|---|
| `README.md` | Rewrite | Overview, features, architecture summary (links to `REFACTORING_GUIDE.md` for the deep dive — no full module map duplicated here), requirements, links to the new getting-started guide, dev/test commands, contributing |
| `REFACTORING_GUIDE.md` | Rewrite | Full accurate module map: `mesh/`'s ~15 collaborators (`Mesh`, `MeshTransport`, `MasterBeacon`, `DownlinkRouter`, `UplinkRouter`, `MeshMessenger`, `RouteReportHandler`, `FrameAuthorizer`, `PeerEnrollment`, `Enrollment`, `PeerRegistry`, `ReplayCache`, `NeighborTable`, `RouteTable`, `E2EKeyStore`, `E2ECrypto`, `RouteMac`, `CompactMessage`), `adapter/`, `persistence/eeprom/`'s 8 domain files, `crypto/Crypto.h`, `network/`, `hardware/`, `error/`, `logging/`, `app/`. Design principles section updated to reflect "encapsulation yes, inheritance sparingly" as the now-established convention. |
| `docs/adapter_development_guide.md` | Rewrite | Current `Adapter` base class API (control-op dispatch table, health-report builders — grown significantly since Phase H2), correct current paths (`src/adapter/pir/PirAdapter`, lowercase, not `src/Adapter/PIR_Adapter/`), correct current `adapter_types` enum (no `LED_ADAPTER` — deleted in Phase C finding 11) |
| `docs/error_codes.md` | Rewrite | Current `lattice::core::ErrorTypeDigit`/`ModuleDigit` registry (grep every real `err::fail`/`err::fatal` call site tree-wide for the actual current code list), current digit-based API usage example (the doc's existing example calls the legacy overload Phase C just deleted) |
| `docs/memory_usage.md` | Rewrite with real measurement | Fresh `idf.py build` + `idf.py size`/`size --files` output via the local ESP-IDF v5.5.1 toolchain — real current flash/RAM numbers, not estimated deltas from a 2026-07-13 baseline. Fixed-RAM allocation breakdown re-derived from current source (the collaborator classes' actual member sizes, not the old single `mesh` object). |
| `docs/server_requirements.md` | Rewrite, cross-checked | Current wire schema — cross-check against the vendored `lattice-protocol` git submodule (`firmware/main/lib/lattice-protocol/`) as ground truth, not just firmware-side usage, since this doc is consumed by an external server team. Current opcodes, current enrollment/E2E-crypto/dual-master/chain-MAC-route-report flow. |
| `docs/hardware_requirements.md` | **New** | Bill of materials (ESP32 board model(s), PIR sensor, TM1637 7-segment display, buttons, LEDs, any resistors/pull-ups needed, power/USB cable), pin assignments cross-checked against `project_config.h`, Mermaid wiring diagrams per component (GitHub renders Mermaid natively in `.md` — no separate artifact needed) |
| `docs/getting_started.md` | **New** | Extremely thorough, non-technical-friendly, **build-from-source** walkthrough: installing ESP-IDF (macOS/Linux/Windows, step by step), cloning the repo + initializing the `lattice-protocol` submodule, configuring `project_config.h` (mesh key generation, adapter selection, WIFI_CHANNEL, peer MACs), `idf.py build`/`idf.py flash`, first-boot provisioning (reading the printed pubkey off serial, what to send the server), verifying it's working (LED pattern meanings, 7-segment display states). Assumes zero prior ESP32/embedded experience — every command shown verbatim, every expected output described. |

**Explicitly deferred, tracked separately:** a pre-built release binary + simple flasher tool (so
end users wouldn't need to build from source at all) — this phase's guide is build-from-source
only, per explicit user decision. File a GitHub issue tracking this as future work before this
phase's PR is opened.

## Approach

**Research-first**, mirroring Phase A's audit structure: parallel survey agents build an accurate
current-state map of each subsystem area before any doc is written, since every one of these docs
was previously wrong in ways that would recur if written from memory/assumption instead of
grep-verified source. Survey areas:
1. `mesh/` — all ~15 collaborators, their responsibilities, and how they compose (for
   `REFACTORING_GUIDE.md` + README's architecture summary)
2. `adapter/` + `hardware/` — current `Adapter` base API, current adapter types, hardware driver
   classes (for `adapter_development_guide.md` + `hardware_requirements.md`'s pin assignments)
3. `persistence/eeprom/` + `crypto/` + `network/` + `error/` + `logging/` + `app/` — the
   remaining module map (for `REFACTORING_GUIDE.md`) and the current error-code registry (for
   `error_codes.md`)
4. Wire protocol — current `mesh_message` fields, opcodes, message types, cross-checked against
   the `lattice-protocol` submodule (for `server_requirements.md`)
5. Build/flash/config — current `project_config.h` reference, current ESP-IDF build commands,
   current `idf.py flash` flow, current first-boot provisioning flow (for `getting_started.md`
   and README's quick-start)

Each survey feeds directly into the doc(s) it informs — no separate "audit findings ledger" is
needed here the way Phase A produced one, since the output is prose to write, not code findings to
bucket into later phases.

**`memory_usage.md`'s measurement step is a real build**, not a survey: `source
~/esp/esp-idf/export.sh && idf.py build` inside `firmware/`, then `idf.py size` and
`idf.py size --files` for the breakdown tables. This must run to completion and produce real
numbers before that doc is written — no estimated figures.

## Review Standard

Unlike code phases, there's no "zero behavior change" bar — the bar is **accuracy against current
source**. Each doc's review must independently re-verify a sample of its concrete claims (file
paths exist, class/function names match current source, code snippets compile against current
signatures, wire-format field lists match `lattice-protocol`, pin numbers match
`project_config.h`) rather than just checking prose reads well. A doc that's well-written but
wrong is worse than the stale docs it replaces — it looks authoritative.

## Global Constraints

Inherited from the umbrella spec: firmware-only scope (docs describe the firmware; no
cross-repo edits to `lattice-hub`/`lattice-protocol` themselves — `server_requirements.md`
consumes the submodule as a read-only reference). No wire-format changes (this phase documents,
never modifies, the protocol). Mermaid diagrams for all wiring/schematic content, embedded
directly in the relevant `.md` files.

## Deliverable note

Before this phase's PR opens, file a GitHub issue (not a code change) tracking "pre-built release
binary + simple flasher tool" as scoped future work, referenced from `getting_started.md` so
readers know it's a known gap, not an oversight.
