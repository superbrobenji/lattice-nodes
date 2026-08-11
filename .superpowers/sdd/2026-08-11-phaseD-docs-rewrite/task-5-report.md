# Task 5 Report: rewrite `docs/memory_usage.md`

## Status
DONE

## What was done

Full rewrite of `docs/memory_usage.md` per the task-5 brief's outline. All six required
sections are present:

1. **Status** — old "re-measurement blocked" framing removed entirely; replaced with a
   note that measurement is unblocked and every number traces to a real `idf.py build`
   run on 2026-08-11.
2. **Build prerequisite** — `master_pubkey_pin.h` `#error` gate explained (with the
   verbatim error text), with a pointer to `docs/getting_started.md` (Task 8) instead
   of duplicating the generation walkthrough.
3. **Flash breakdown** — real `idf.py size` summary table (verbatim), the full
   `idf.py size-components` per-archive table (verbatim), plus a filtered
   `idf.py size-files` table of application object files only (main.cpp.obj,
   Mesh.cpp.obj, MeshMessenger.cpp.obj, etc.), all copied from the research file's real
   captured output.
4. **RAM breakdown** — IRAM/DRAM/RTC-SLOW usage table from the same `idf.py size` run,
   plus the research's caveat that the DRAM "free" figure is static/link-time headroom,
   not runtime free heap.
5. **Fixed allocations by collaborator** — a table covering `RouteTable`, `E2EKeyStore`,
   `PeerRegistry`, `ReplayCache`, `NeighborTable`, `DownlinkRouter`'s LRU,
   `PendingRelayQueue`, `MeshTransport`'s RX ring, and `OutboundSequenceState`, sourced
   from `phaseD-research-mesh.md`'s collaborator list plus config bounds independently
   confirmed in `phaseD-research-build-flash-config.md` Part 2's `project_config.h`
   survey (and `MAX_PEERS=10` from this session's persistence-layer research file).
6. **Re-measure instructions** — exact command sequence (`export.sh` → `idf.py build` →
   `idf.py size`/`size-components`/`size-files`), noting the `master_pubkey_pin.h`
   prerequisite and that there's no CI gate keeping this doc honest, so it's a manual
   step after mesh/config/dependency changes.

## Factual-accuracy self-assessment

Every flash/RAM number in §3–§4 is copied verbatim (not retyped/recalculated) from
`phaseD-research-build-flash-config.md`'s Part 1 (real captured `idf.py
build`/`size`/`size-components`/`size-files` output), except the hex→decimal
conversions (`0xa22b0`, `0x100000`, `0x5dd50`, `0x6680`, `0x980`), which I computed with
`python3` and cross-checked against the research's own stated percentages (37% free
free/total = 36.65%, rounds to 37% — consistent). The §5 collaborator table's capacity
bounds (`LATTICE_ROUTE_TABLE_MAX=16`, `LATTICE_E2E_KEYCACHE_MAX=10`/`_LEAF=2`,
`LATTICE_REPLAY_MAX_ORIGINS=12`, `LATTICE_NEIGHBOR_MAX=8`, `LATTICE_DOWNLINK_PEER_MAX=4`,
`MAX_PEERS=10`) are all taken directly from the research files' own read-only source
survey — none invented.

## Concerns / things flagged rather than guessed

- **Per-struct byte sizes for §5's collaborators are explicitly marked "not confirmed
  this pass"** for every collaborator except the flagged RouteTable discrepancy below.
  Neither research file gives field-level struct layouts (only architecture-level
  prose), so I did not hand-compute sizes from source headers myself — the outer
  instruction was explicit that if a precise byte count isn't derivable from the
  research, the doc should say so rather than guess, so I followed that literally
  instead of independently reading `firmware/main/src/mesh/*.h` to compute my own
  numbers (which risked introducing exactly the kind of unverified figure this task
  exists to eliminate).
- **Caught and flagged a stale-number propagation risk**: `phaseD-research-mesh.md`
  mentions RouteTable's size in passing as "~2.25KB", but this session's own Part 2
  research confirmed `LATTICE_ROUTE_TABLE_MAX` is now **16**, not the **32** the old
  2026-07-13 baseline assumed when it originally computed 2.25KB. Since 16×(same
  per-entry size) ≠ 32×(same per-entry size), I did not republish "~2.25KB" as current
  — I called out the discrepancy explicitly in the doc instead of silently repeating a
  number that no longer matches the confirmed bound.
- `docs/getting_started.md` does not yet exist in this worktree (it's Task 8's
  deliverable, presumably being written in a sibling worktree) — I reference it
  prospectively per the brief's instruction, without assuming its final section
  structure.
- This doc's outline (per the brief) does not include an EEPROM/NVS persistence
  section; the old doc had one. I did not carry it forward or fabricate a
  replacement, since it's out of this task's scope and appears to belong to a
  different task/doc (a separate research file, `phaseD-research-persistence-crypto-error.md`,
  covers that ground).
