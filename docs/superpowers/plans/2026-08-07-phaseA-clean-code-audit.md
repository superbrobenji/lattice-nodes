# Phase A — Clean-Code Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Produce a ranked findings ledger auditing every file in `firmware/main/src` for excessive size, SRP violations, missing encapsulation, real (vs. artificial) inheritance opportunities, and library-replacement candidates — bucketed into Phase B (Mesh.cpp), Phase C (repo-wide sweep), and any new phases large findings warrant.

**Architecture:** Task 1 (file census) creates the ledger doc and gates everything else. Tasks 2-4 (mesh/ subsystem, non-mesh subsystems, library/architecture scan) are fully independent of each other — each reads its own file set and writes its own standalone fragment file under `docs/superpowers/specs/.audit-fragments/`, so they touch no file in common and can run in parallel worktrees with zero merge risk. Task 5 (synthesis) reads all three fragments plus the census, merges everything into the final ledger, and deletes the fragments. Tasks 1-4 are read-only — zero source-code changes in this phase.

**Tech Stack:** Markdown ledger doc (mirrors `docs/superpowers/specs/2026-08-04-post-phaseG-audit-findings.md` format), `wc`/`grep`/`find` for verification, no build/test toolchain needed since no source is modified.

## Global Constraints

- **Firmware-only.** No `lattice-hub` or `lattice-protocol` touches.
- **No wire-format changes, no code changes at all in this phase.** Phase A is read-only investigation; the unit/e2e regression requirement from the umbrella spec starts applying at Phase B, not here.
- **No line-count threshold.** Every file in `firmware/main/src` gets sized and judged on responsibility count — a 300-line file doing 4 unrelated things is as much a finding as a 1000-line one.
- **Encapsulation yes, inheritance sparingly** — every inheritance-opportunity finding must name the real polymorphic need it removes (the existing `Adapter`/`PirAdapter`/`SerialAdapter` split is the reference example of "real"); flag anything that would just add a vtable for no behavioral gain as **not** a finding.
- **Library-caution clause.** Every library-replacement candidate must carry an estimated flash/RAM delta, not just a suitability note — Phase J reverted a libsodium swap that cost 92.5 KB more than the code it replaced.
- **Exclude vendored/generated code:** `firmware/main/src/mesh/serialization/nanopb/` (vendored nanopb library) and `mesh.pb.h`/`mesh.pb.c` (generated from `lattice-protocol`) are out of scope — not our code, not ours to restructure.
- **Exclude stray worktrees:** `.claude/worktrees/*` contains leftover checkouts from prior subagent runs — never let a `find`/`wc` command run from repo root pick these up. All commands in this plan are scoped to `firmware/main/src` specifically, which sidesteps this.

---

### Task 1: File-size & responsibility census

**Files:**
- Create: `docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md` (new ledger doc — this task creates it with the census section; Tasks 2-4 write independent fragment files, not this file; Task 5 merges the fragments into this file and finalizes it)

**Interfaces:**
- Produces: a `## File census` section with one markdown table, columns `File | Lines | Flag`, covering every file in scope. `Flag` is `investigate` (this file gets a deep look in Task 2/3) or `-` (small/simple, no deep look needed). This table is the input Tasks 2-4 read to decide where to look closely, and Task 5 reads to sanity-check nothing large was skipped.

- [ ] **Step 1: Run the full-tree line count, scoped and excluded correctly**

```bash
cd /Users/benji/projects/personal/lattice-nodes
find firmware/main/src \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
  ! -path "*/serialization/nanopb/*" ! -name "mesh.pb.h" ! -name "mesh.pb.c" \
  | xargs wc -l | sort -rn
find firmware/main -maxdepth 1 \( -name "*.cpp" -o -name "*.h" \) | xargs wc -l
```

Expected: a ranked list headed by `Mesh.cpp` (~1382 lines), with `main.cpp` (~579) and `project_config.h` (~185) from the second command folded in.

- [ ] **Step 2: Write the census table into the ledger doc**

```markdown
# Clean-Code Audit — Findings Ledger

**Status:** Reference document (not an implementation spec).
**Date:** 2026-08-07
**Method:** Phase A of `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md`. File census + 2 parallel subsystem audits (mesh, non-mesh) + 1 library/architecture scan.
**Purpose:** capture every finding with a stable ID, bucket, and disposition (Phase B / Phase C / new phase / keep-as-is).

## File census

| File | Lines | Flag |
|---|---|---|
| `firmware/main/src/mesh/Mesh.cpp` | 1382 | investigate |
| ... (one row per file from Step 1's output, every file, not a filtered subset) ... |
```

Set `Flag = investigate` for every file over ~150 lines, **and** for any file under that where you already know (from Task 1's own read of the file list) it mixes unrelated concerns by name alone (e.g. a file whose name implies one job but that historically grew a second) — judgment call, not a hard cutoff, consistent with the no-threshold constraint. When in doubt, flag it; Tasks 2-4 deciding "nothing here" costs a few minutes, missing a real finding costs a phase.

- [ ] **Step 3: Verify the table is complete**

```bash
# Count files in the census table vs. files found on disk — must match.
grep -c '^| `firmware' docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md
find firmware/main/src \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
  ! -path "*/serialization/nanopb/*" ! -name "mesh.pb.h" ! -name "mesh.pb.c" | wc -l
```

Expected: the two counts match (plus 2 for `main.cpp`/`project_config.h`). If they don't, a file was dropped from the table — find it and add it.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md
git commit -m "docs(phaseA): file census"
```

---

### Task 2: mesh/ subsystem audit

**Runs in parallel with Task 3 and Task 4** (independent file sets, independent output file — safe to dispatch in a separate worktree).

**Files:**
- Create: `docs/superpowers/specs/.audit-fragments/mesh-findings.md` (standalone fragment — do not touch the main ledger doc; Task 5 merges this in)
- Read (in scope, exclude `serialization/nanopb/` and `mesh.pb.h`):
  - `firmware/main/src/mesh/Mesh.{h,cpp}`
  - `firmware/main/src/mesh/Enrollment.{h,cpp}`
  - `firmware/main/src/mesh/PeerRegistry.{h,cpp}`
  - `firmware/main/src/mesh/NeighborTable.h`
  - `firmware/main/src/mesh/RouteTable.h`
  - `firmware/main/src/mesh/ReplayCache.h`
  - `firmware/main/src/mesh/E2EKeyStore.h`
  - `firmware/main/src/mesh/E2ECrypto.h`
  - `firmware/main/src/mesh/MeshCrypto.h`
  - `firmware/main/src/mesh/RouteMac.h`
  - `firmware/main/src/mesh/CompactMessage.{h,cpp}`
  - `firmware/main/src/mesh/broadcast_mac.h`

**Interfaces:**
- Consumes: nothing from other tasks — the file list above is exhaustive and hardcoded, no need to read Task 1's census output to know what to read.
- Produces: `docs/superpowers/specs/.audit-fragments/mesh-findings.md`, containing one markdown table headed `## Mesh subsystem findings`, columns `ID | File:line | Category | Description | Suggested direction | Effort`, IDs prefixed `MESH-` (`MESH-1`, `MESH-2`, ...). `Category` is one of `size/SRP`, `encapsulation`, `inheritance`, `library`. Task 5 reads this fragment for the master ranking.

- [ ] **Step 1: Read every flagged file, per-class responsibility breakdown**

For each class in scope, list every distinct job it does (a "job" is a cohesive group of methods that could move to another class without the rest noticing). `Mesh` is already known to do 6 jobs beyond orchestration (transport, dispatch, beacon, downlink routing, key persistence, replay guarding — see the umbrella spec's Phase B notes) — confirm that breakdown still holds by reading the current file, and apply the same per-class breakdown to every other flagged file in this list (`Enrollment`, `PeerRegistry`, `NeighborTable`, `RouteTable`, `ReplayCache`, `E2EKeyStore` are the other stateful classes; the `*Crypto.h`/`RouteMac.h`/`CompactMessage.*` files are smaller and more likely single-purpose, confirm rather than assume).

- [ ] **Step 2: Check encapsulation on every class found in Step 1**

For each class: are its data members `private`/`protected`, or does something outside the class reach into them directly? Grep for the class's member variable names outside its own `.h`/`.cpp` pair to check:

```bash
# Example for one member — repeat per public/protected data member found in Step 1.
grep -rn "\.downlinkPeerLru\b\|->downlinkPeerLru\b" firmware/main/src --include=*.cpp --include=*.h
```

- [ ] **Step 3: Check inheritance opportunities**

The only existing inheritance in this subsystem is implicit (none — `mesh/` classes are all concrete, non-derived). Note explicitly for each class whether a base class would remove real duplication (e.g. the shared "linear-scan-by-MAC" skeleton `NeighborTable`/`RouteTable`/`E2EKeyStore`/`ReplayCache`/`PeerRegistry` already partially dedup via `mac_table::` free functions per Phase H2 item Y — check whether that's a case for a shared base class instead, or whether the free-function form is already the right call per this effort's "inheritance sparingly" constraint) — and say so if the answer is "no, free functions are already correct" rather than omitting it.

- [ ] **Step 4: Write the findings table into the fragment file, verify every citation**

Write the `## Mesh subsystem findings` table as the full content of `docs/superpowers/specs/.audit-fragments/mesh-findings.md`. For every `File:line` cited, grep-confirm the line number is still current:

```bash
sed -n '<line>p' firmware/main/src/mesh/<File>
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/.audit-fragments/mesh-findings.md
git commit -m "docs(phaseA): mesh subsystem findings"
```

---

### Task 3: Non-mesh subsystem audit

**Runs in parallel with Task 2 and Task 4** (independent file sets, independent output file — safe to dispatch in a separate worktree).

**Files:**
- Create: `docs/superpowers/specs/.audit-fragments/nonmesh-findings.md` (standalone fragment — do not touch the main ledger doc or Task 2's fragment; Task 5 merges this in)
- Read:
  - `firmware/main/src/adapter/{Adapter,AdapterFactory}.{h,cpp}`, `firmware/main/src/adapter/pir/PirAdapter.{h,cpp}`, `firmware/main/src/adapter/serial/{SerialAdapter,SerialFraming}.{h,cpp}`
  - `firmware/main/src/hardware/input/{Button,GpioInput,Pir}.{h,cpp}`, `firmware/main/src/hardware/output/{GpioOutput,Led,SevenSegDisplay}.{h,cpp}`
  - `firmware/main/src/app/{BootManager,ButtonHandler,DisplayManager}.h`
  - `firmware/main/src/persistence/EepromManager.{h,cpp}`
  - `firmware/main/src/error/{Error,ErrorCodes,ErrorCore}.{h,cpp}`
  - `firmware/main/src/logging/Logger.{h,cpp}`
  - `firmware/main/src/network/{hw_mac,mac_table,MacAddress,MacEq,mem}.h`
  - `firmware/main/src/crypto/Crypto.h`
  - `firmware/main/main.cpp`, `firmware/main/project_config.h`

**Interfaces:**
- Consumes: nothing from other tasks — the file list above is exhaustive and hardcoded.
- Produces: `docs/superpowers/specs/.audit-fragments/nonmesh-findings.md`, same table schema as Task 2 (`ID | File:line | Category | Description | Suggested direction | Effort`), IDs prefixed `SYS-`. Task 5 reads this fragment.

- [ ] **Step 1: Read every flagged file, per-class/per-file responsibility breakdown**

Same method as Task 2 Step 1. `main.cpp` (579 lines) and `EepromManager.cpp` (651 lines) are the known large ones — confirm what jobs each does. `main.cpp` in particular likely mixes boot sequencing, WiFi/ESP-NOW setup delegation, main loop, and adapter wiring — break it down explicitly rather than treating it as one "setup/loop" blob.

- [ ] **Step 2: Check encapsulation, same method as Task 2 Step 2**

Pay particular attention to `Adapter`/`PirAdapter`/`SerialAdapter` — this is the one subsystem that already uses inheritance correctly (per the umbrella spec's reference example). Confirm that's still true (no public data members bypassing the interface) rather than assuming it from the umbrella spec's note.

- [ ] **Step 3: Check inheritance — both directions**

Two things to look for here, since this is the subsystem most likely to have both problems:
1. **Missing inheritance**: `GpioInput`/`GpioOutput` and their concrete users (`Button`, `Pir`, `Led`) — is there duplication a shared base would remove, and if so is it a *real* need or would it just add a vtable (Phase G item I already removed `virtual` from these because it was never dispatched polymorphically — check whether that reasoning still holds before proposing to re-add it).
2. **Misused/artificial inheritance**: anywhere a base class exists but its derived classes don't actually get treated polymorphically (called only through their concrete type, never through a base pointer) — that's inheritance for no behavioral gain and is itself a finding (simplify to composition or free functions).

- [ ] **Step 4: Write the findings table into the fragment file, verify every citation**

Write the `## Non-mesh subsystem findings` table as the full content of `docs/superpowers/specs/.audit-fragments/nonmesh-findings.md`. Same citation-verification method as Task 2 Step 4.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/.audit-fragments/nonmesh-findings.md
git commit -m "docs(phaseA): non-mesh subsystem findings"
```

---

### Task 4: Library-replacement candidates + architecture-boundary reference

**Runs in parallel with Task 2 and Task 3** (independent scope, independent output file — safe to dispatch in a separate worktree). Unlike a typical Task 4, this one does **not** wait on Tasks 2-3: it's a fresh repo-wide grep sweep plus a structural comparison against `lattice-hub`, neither of which needs Task 2/3's findings. (The keep-as-is list, which does need cross-referencing against what Tasks 2-3 found, is compiled in Task 5 instead — see that task's Step 1a.)

**Files:**
- Create: `docs/superpowers/specs/.audit-fragments/library-findings.md` (standalone fragment — do not touch the main ledger doc or Tasks 2-3's fragments; Task 5 merges this in)
- Read (repo-wide grep, not a fixed file list — see Step 1): `firmware/main/src/**/*.{h,cpp}`
- Read (for the architecture-boundary comparison, structure only, not line-by-line): `lattice-hub/server/` top-level package layout (`orchestrator`, `sidecar`, `dashboard`, `artist-portal`) if the sibling repo is available at `/Users/benji/projects/personal/lattice-hub`; if not available, skip this section and note it as skipped rather than guessing at hub's structure from memory.

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: `docs/superpowers/specs/.audit-fragments/library-findings.md`, containing `## Library candidates` table (`ID | Hand-rolled code | Candidate library | Est. flash/RAM delta | Verdict`, IDs prefixed `LIB-`) and `## Architecture-boundary reference` (prose, no IDs). Task 5 reads this fragment.

- [ ] **Step 1: Scan for library-replacement candidates**

Grep for hand-rolled patterns that commonly have an embedded-C++ library equivalent — ring buffers, state machines, fixed-size hash/lookup tables, checksum/CRC routines:

```bash
grep -rn "class.*Table\|struct.*Ring\|switch.*State\|enum.*State" firmware/main/src --include=*.h --include=*.cpp
```

For each candidate found, name the specific library (e.g. an ESP-IDF-native equivalent, not a generic suggestion), and give an estimated flash/RAM delta using the same method Phase I's audit used (compare against what the library actually pulls in, not just its own size — libsodium's real cost was the *whole* library linked in, not just the functions used). If no credible estimate is possible without actually trying the swap, mark `Verdict = needs-spike` rather than guessing a number.

- [ ] **Step 2: Compare against lattice-hub's package layering**

```bash
ls /Users/benji/projects/personal/lattice-hub/server/ 2>/dev/null
```

If present, note in 3-5 sentences whether nodes' subsystem split (`mesh/`, `adapter/`, `hardware/`, `persistence/`, `app/`) already mirrors hub's boundary discipline (each package owns its own concerns, talks to others through narrow interfaces) or where nodes' boundaries are muddier (e.g. `Mesh.cpp` reaching directly into `EepromManager` rather than through a narrower persistence interface). If the hub repo isn't present at that path, write `Architecture-boundary reference: skipped — lattice-hub not available locally` and move on.

- [ ] **Step 3: Write both sections into the fragment file, verify citations**

Write `## Library candidates` and `## Architecture-boundary reference` as the full content of `docs/superpowers/specs/.audit-fragments/library-findings.md`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/.audit-fragments/library-findings.md
git commit -m "docs(phaseA): library candidates + architecture reference"
```

---

### Task 5: Synthesis — rank, assign final IDs, bucket into phases, update umbrella spec

**Runs after Tasks 1-4 all complete** (only sequential task besides Task 1 — reads every fragment).

**Files:**
- Modify: `docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md` (merge in the three fragments' content; add `## Bucket assignments` and `## Keep-as-is` sections per the prior audit's format; renumber `MESH-*`/`SYS-*`/`LIB-*` into one final sequential ID space)
- Read: `docs/superpowers/specs/.audit-fragments/mesh-findings.md`, `nonmesh-findings.md`, `library-findings.md`
- Delete: `docs/superpowers/specs/.audit-fragments/` (whole directory, once its contents are folded into the main ledger)
- Modify: `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (only if a finding is large enough to warrant a new phase per the "phase list is not closed" note — add the new phase row + update the dependency graph)

**Interfaces:**
- Consumes: every row from the three fragment files, plus the `## File census` table already in the main ledger from Task 1.
- Produces: final ledger with one merged table, IDs `1`, `2`, `3`, ... in descending priority order (highest maintainability-impact-for-effort first, same convention as the prior audit's flash-savings-first order). Each row tagged with its bucket: `Phase B` (Mesh.cpp-specific), `Phase C` (repo-wide sweep), `Phase <new letter>` (its own phase — only for findings at or near the size/complexity of the known Mesh.cpp case), or `Keep-as-is`.

- [ ] **Step 1: Merge and renumber**

Pull every row from the three fragment files' tables (`Mesh subsystem findings`, `Non-mesh subsystem findings`, `Library candidates`) into one list, and append the `Architecture-boundary reference` prose from the library fragment as its own section. Assign final sequential IDs in priority order — judge priority by (maintainability impact) ÷ (effort), same heuristic the prior audit used for flash savings. Add a `## Bucket assignments` section listing which ID range/set went to which bucket (mirrors `2026-08-04-post-phaseG-audit-findings.md`'s bucket-assignments section).

- [ ] **Step 1a: Compile the keep-as-is list**

Cross-reference the `## File census` table's `investigate`-flagged rows against the merged findings: any flagged file with zero rows pointing at it was read by Task 2 or 3, judged, and found to already be correct for the embedded constraints. List each one in a `## Keep-as-is` section with a one-line reason (pull the reason from context if the fragment's per-file breakdown already stated one; otherwise this step's implementer reads the file directly to write an accurate one-liner) — mirrors the prior audit's "Keep-as-is" format.

- [ ] **Step 1b: Delete the fragments directory**

```bash
git rm -r docs/superpowers/specs/.audit-fragments/
```

- [ ] **Step 2: Decide if any finding needs its own new phase**

A finding warrants a new phase (not folded into Phase C) if it clears the same bar `Mesh.cpp` did for Phase B — meaningfully large in isolation (roughly, changes to a single file/class cluster large enough that a reviewer would want it as its own PR-sized effort, not one line in a repo-wide sweep). If none do, state that explicitly in the ledger rather than leaving it implicit.

- [ ] **Step 3: If a new phase is warranted, update the umbrella spec**

Add a new row to the Phase map table in `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (next letter after D — this effort's own sequence, per the "clean new unrelated effort" decision, starts at A and continues from there), and add it to the dependency graph as gated by Phase A, parallel to B/C. Skip this step (leave the umbrella spec untouched) if Step 2 found nothing large enough.

- [ ] **Step 4: Final read-through**

Read the complete merged ledger doc top to bottom. Confirm: every ID cited in the Bucket assignments section exists in the main table; every `File:line` citation was already grep-verified in Tasks 2-4; no `TBD`/placeholder text remains.

- [ ] **Step 5: Commit, push, open PR**

```bash
git add docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md
git commit -m "docs(phaseA): synthesis — ranked findings, phase bucket assignments, fragment cleanup"
git push -u origin docs/clean-code-refactor-umbrella-spec
gh pr create --title "docs(phaseA): Clean-Code audit findings" --body "$(cat <<'EOF'
## Summary
- Phase A of the clean-code-refactor umbrella: full-tree file census + mesh/non-mesh subsystem audits + library-replacement scan.
- Findings ranked and bucketed into Phase B (Mesh.cpp decomposition), Phase C (repo-wide sweep), and any new phases warranted.

## Test plan
- [ ] No code changes in this PR — docs only.
- [ ] Every File:line citation grep-verified against current main.
EOF
)"
```

---

## Self-Review Notes

- **Spec coverage:** Task 1 covers the umbrella spec's "no line-count threshold, every file sized" requirement. Tasks 2-3 cover the subsystem list from the umbrella spec's Phase A row (`mesh/`, `adapter/`, `hardware/`, `app/`, `persistence/`, `error/`, `logging/`, `network/`, `crypto/`) plus `main.cpp`/`project_config.h`. Task 4 covers library-replacement candidates and the hub architecture-boundary reference, both named in the umbrella spec. Task 5 covers the "phase list is not closed" requirement and the Phase B/C bucketing.
- **Placeholder scan:** every step names exact files, exact commands, or exact table schemas rather than "note findings" — the specific finding *content* is necessarily produced during execution (this is an investigation phase, not a code-change phase), but the deliverable shape, scope, and verification method are fully specified.
- **Type/schema consistency:** all three subsystem tasks use the same table schema (`ID | File:line | Category | Description | Suggested direction | Effort`) so Task 5's merge step doesn't need to reconcile mismatched columns.
- **Parallelism:** Tasks 2-4 were restructured to write independent fragment files instead of appending to a shared ledger, specifically so they can be dispatched in parallel worktrees without merge conflicts (three branches each adding one new, distinct file merge cleanly; three branches each appending to the end of the same file do not). Task 4's dependency on Tasks 2-3's IDs was removed by moving keep-as-is compilation to Task 5, which already reads everything.
