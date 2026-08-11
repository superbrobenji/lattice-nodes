# Task 1 Report — `REFACTORING_GUIDE.md` rewrite

**Status:** DONE

**Commit:** `4307bb3` on branch `phaseD-task1-manual` ("docs(phaseD): rewrite REFACTORING_GUIDE.md for the post-A/B/C/E architecture")

## Note on brief location

The controller pointed at `.superpowers/sdd/2026-08-11-phaseD-docs-rewrite/task-1-brief.md`,
which did not exist in this worktree (that directory didn't exist at all before this report). The
worktree's tracked `docs/superpowers/plans/2026-08-11-phaseD-docs-rewrite.md` contains the full
Task 1 brief inline (content outline, "must get right" list, research file pointers), so I used
that as the requirements source instead — consistent with the plan's own contingency note ("copy
this plan file's task briefs into each worktree explicitly if they don't travel via `git worktree
add`"). No content was guessed; the brief text was fully available, just at a different path.

## What was done

Full rewrite of `REFACTORING_GUIDE.md`, replacing the stale monolithic-`Mesh`/`EepromManager`
description with an accurate current-state map, per the brief's outline:

1. **Design Principles** — Tiger Style, plus a source-verified three-tier explanation of
   "encapsulation yes, inheritance sparingly": (a) `Adapter`→`PirAdapter`/`SerialAdapter` as the
   one deliberate *polymorphic* (virtual-dispatch) inheritance use; (b) `GpioInput`→`Button`/`Pir`
   and `GpioOutput`→`Led` as a distinct, explicitly non-virtual structural-reuse pattern (verified
   by reading `GpioInput.h`'s own header comment and the actual class declarations); (c)
   `network/mac_table.h`'s free-function `find`/`evict_oldest_by_ts` skeleton as the deliberate
   non-inheritance choice shared by 5 MAC-keyed tables (`NeighborTable`, `RouteTable`,
   `E2EKeyStore`, `ReplayCache`, `PeerRegistry`) — read the actual header to confirm this.
2. **Module Map — `mesh/`** — all ~16 collaborators, grouped exactly as the research groups them
   (radio/transport, peer/identity, routing, messaging/send-pipeline, security/crypto), each with
   responsibility + key API. The `Mesh.h`/`Mesh.cpp` orchestrator section reproduces the research's
   method-by-method table plus its "net assessment" paragraph. Also added a short accurate note on
   `serialization/` (vendored nanopb, not used by the mesh radio protocol — used by
   `SerialAdapter`/`SerialFraming` instead), which the old doc got subtly wrong.
3. **Module Map — `adapter/` + `hardware/`** — condensed base-class API, factory, `PirAdapter`,
   `SerialAdapter`, hardware driver classes; points to `docs/adapter_development_guide.md` for
   depth.
4. **Module Map — `persistence/eeprom/`** — 8-file table, explicitly states no facade/re-export
   header exists and the flat `lattice::eeprom` namespace.
5. **Module Map — `crypto/`, `network/`, `error/`, `logging/`, `app/`** — one paragraph each,
   condensed from the research.
6. **"Adding a Module"** checklist — SRP, `Error.h` digit-based API (explicitly warns off the
   deleted legacy overload), `GpioInput`/`GpioOutput` for new drivers, reserve-at-boot, and the
   CMakeLists dual-registration requirement (`firmware/main/CMakeLists.txt` SRCS +
   `tests/CMakeLists.txt` FIRMWARE_SOURCES).

## Verification performed

- Confirmed `Mesh.h`/`Mesh.cpp` = 421 + 442 = 863 lines via `wc -l` on the actual files in this
  worktree (matches the research and the brief's "must get right" note).
- Confirmed via `find`/`ls` that `EepromManager.h`/`.cpp` and `MacAddress.h` do not exist anywhere
  in the tree, and that no `LED_ADAPTER` exists in the firmware-side `adapter_types` enum.
- Read `firmware/main/src/network/mac_table.h`, `hardware/input/{Button,GpioInput,Pir}.h`,
  `hardware/output/{Led,GpioOutput,SevenSegDisplay}.h`, and `adapter/{Adapter,pir/PirAdapter,
  serial/SerialAdapter}.h` directly to verify the inheritance-pattern claims in the Design
  Principles section (this was the one point the brief flagged as worth an independent check
  rather than pure transcription).
- Cross-checked `firmware/main/CMakeLists.txt` and `tests/CMakeLists.txt` directly to confirm the
  dual-registration claim in the "Adding a Module" checklist (both files list the same `mesh/`,
  `adapter/`, `persistence/eeprom/` sources, confirming the pattern).
- **Found and corrected one error in the brief itself**: the brief's checklist item says
  "the `mesh_table.h` free-function pattern" — the actual file is `network/mac_table.h`
  (`namespace lattice::mac_table`), confirmed by `ls` and by reading the file. Used the correct
  name throughout the doc.
- Self-check per the brief: extracted every `docs/*.md`, `tests/*.cpp`, and `*.h`/`*.cpp` filename
  token cited in the new doc and confirmed each exists in the current tree via `find`. All real
  citations resolved; the only non-matches were regex artifacts (splitting `E2ECrypto.h` etc. on
  the leading digit) and the intentionally-cited-as-deleted `MacAddress.h`.
- `firmware/main/lib/lattice-protocol` (git submodule) is not checked out in this worktree
  (empty directory), so the one path reference to it (`mesh_message` struct location) relies on
  the research file's grep-verified claim rather than independent verification in this session —
  flagged here per the brief's guidance to favor accuracy over completeness.

## Self-assessment

Factual accuracy: verified directly against source for every claim in the Design Principles
section (the part most likely to be wrong if taken purely from memory) and for all structural
facts (line counts, enum values, deleted files, CMakeLists contents). The bulk of the `mesh/`
module map content was transcribed/condensed from the grep-verified research file per the brief's
instructions, with spot-checks against actual file listings (`ls firmware/main/src/mesh`,
`firmware/main/src/persistence/eeprom`, etc.) to confirm every filename exists. No numeric claim
(line counts, error codes, pin numbers) was invented — all come from the research or a live `wc
-l`/`ls`/`find`/`grep` in this session.

## Concerns

None blocking. Two minor notes for the controller:
- The brief's own text has a typo (`mesh_table.h` should be `mac_table.h`) — corrected silently in
  the doc, noted here for the record.
- The `.superpowers/sdd/2026-08-11-phaseD-docs-rewrite/` directory did not exist in this worktree
  before I created it for this report — if other parallel tasks expect to find their own
  `task-N-brief.md` there, they'll hit the same gap and should fall back to
  `docs/superpowers/plans/2026-08-11-phaseD-docs-rewrite.md` as I did.
