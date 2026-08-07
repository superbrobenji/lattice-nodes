# Clean-Code Audit — Findings Ledger

**Status:** Reference document (not an implementation spec).
**Date:** 2026-08-07
**Method:** Phase A of `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md`. File census + 2 parallel subsystem audits (mesh, non-mesh) + 1 library/architecture scan.
**Purpose:** capture every finding with a stable ID, bucket, and disposition (Phase B / Phase C / new phase / keep-as-is).

## File census

Scope: `firmware/main/src/**/*.{cpp,h,hpp}` (excluding `src/mesh/serialization/nanopb/` and the
generated `mesh.pb.h`/`mesh.pb.c`) plus the two top-level files `firmware/main/main.cpp` and
`firmware/main/project_config.h`. 57 files total (55 in `src/` + 2 top-level).

`Flag = investigate` is set for every file over ~150 lines, plus a small number of files under
that line count flagged on a judgment call (name/content suggests a second concern, or a
documented dead/overlapping-responsibility smell) — see Task 1 report for rationale on each.
This is not a hard-cutoff rule; Tasks 2-4 should still use their own judgment within flagged
files and may flag additional files if they find something during the deep look.

| File | Lines | Flag |
|---|---|---|
| `firmware/main/src/mesh/Mesh.cpp` | 1382 | investigate |
| `firmware/main/src/persistence/EepromManager.cpp` | 651 | investigate |
| `firmware/main/main.cpp` | 579 | investigate |
| `firmware/main/src/mesh/Mesh.h` | 495 | investigate |
| `firmware/main/src/adapter/serial/SerialAdapter.cpp` | 310 | investigate |
| `firmware/main/src/adapter/Adapter.cpp` | 245 | investigate |
| `firmware/main/src/mesh/Enrollment.cpp` | 224 | investigate |
| `firmware/main/src/adapter/serial/SerialFraming.cpp` | 209 | investigate |
| `firmware/main/src/mesh/NeighborTable.h` | 203 | investigate |
| `firmware/main/src/hardware/output/SevenSegDisplay.cpp` | 200 | investigate |
| `firmware/main/project_config.h` | 185 | investigate |
| `firmware/main/src/mesh/PeerRegistry.cpp` | 184 | investigate |
| `firmware/main/src/hardware/output/Led.cpp` | 162 | investigate |
| `firmware/main/src/crypto/Crypto.h` | 159 | investigate |
| `firmware/main/src/persistence/EepromManager.h` | 156 | investigate |
| `firmware/main/src/error/ErrorCore.cpp` | 142 | - |
| `firmware/main/src/adapter/Adapter.h` | 140 | - |
| `firmware/main/src/adapter/pir/PirAdapter.cpp` | 138 | - |
| `firmware/main/src/app/ButtonHandler.h` | 126 | investigate |
| `firmware/main/src/logging/Logger.h` | 117 | - |
| `firmware/main/src/mesh/E2ECrypto.h` | 110 | - |
| `firmware/main/src/mesh/E2EKeyStore.h` | 103 | - |
| `firmware/main/src/error/Error.h` | 103 | - |
| `firmware/main/src/adapter/AdapterFactory.cpp` | 103 | - |
| `firmware/main/src/mesh/CompactMessage.h` | 96 | - |
| `firmware/main/src/mesh/ReplayCache.h` | 92 | - |
| `firmware/main/src/mesh/Enrollment.h` | 89 | - |
| `firmware/main/src/mesh/RouteTable.h` | 88 | - |
| `firmware/main/src/adapter/serial/SerialAdapter.h` | 88 | - |
| `firmware/main/src/network/mac_table.h` | 81 | - |
| `firmware/main/src/logging/Logger.cpp` | 79 | - |
| `firmware/main/src/mesh/RouteMac.h` | 74 | - |
| `firmware/main/src/network/hw_mac.h` | 71 | - |
| `firmware/main/src/error/ErrorCore.h` | 68 | - |
| `firmware/main/src/adapter/pir/PirAdapter.h` | 68 | - |
| `firmware/main/src/hardware/input/Pir.cpp` | 66 | - |
| `firmware/main/src/app/DisplayManager.h` | 65 | - |
| `firmware/main/src/hardware/output/Led.h` | 60 | - |
| `firmware/main/src/mesh/PeerRegistry.h` | 57 | - |
| `firmware/main/src/adapter/AdapterFactory.h` | 50 | - |
| `firmware/main/src/network/MacAddress.h` | 49 | investigate |
| `firmware/main/src/mesh/CompactMessage.cpp` | 49 | - |
| `firmware/main/src/hardware/input/Pir.h` | 48 | - |
| `firmware/main/src/mesh/MeshCrypto.h` | 47 | - |
| `firmware/main/src/adapter/serial/SerialFraming.h` | 46 | - |
| `firmware/main/src/hardware/output/SevenSegDisplay.h` | 45 | - |
| `firmware/main/src/hardware/input/Button.cpp` | 42 | - |
| `firmware/main/src/hardware/input/Button.h` | 36 | - |
| `firmware/main/src/app/BootManager.h` | 35 | - |
| `firmware/main/src/hardware/input/GpioInput.h` | 32 | - |
| `firmware/main/src/hardware/input/GpioInput.cpp` | 32 | - |
| `firmware/main/src/hardware/output/GpioOutput.cpp` | 31 | - |
| `firmware/main/src/hardware/output/GpioOutput.h` | 28 | - |
| `firmware/main/src/error/ErrorCodes.h` | 27 | - |
| `firmware/main/src/network/MacEq.h` | 25 | - |
| `firmware/main/src/network/mem.h` | 21 | - |
| `firmware/main/src/mesh/broadcast_mac.h` | 16 | - |

### Judgment calls on sub-150-line flags

- **`firmware/main/src/app/ButtonHandler.h` (126 lines, flagged):** highest fan-in of any
  file under the size threshold — pulls in `Mesh`, `EepromManager`, `Led`, `Button`, and
  `Logger` to coordinate a hold-to-configure/reset gesture across hardware, mesh, and
  persistence layers in one static `tick()`. Cohesive in intent (it's the app's one button
  orchestrator) but broad enough in responsibility, and close enough to the size threshold,
  that Task 3 (non-mesh audit) should give it a real look rather than a skim.
- **`firmware/main/src/network/MacAddress.h` (49 lines, flagged):** `MacEq.h`'s own header
  comment documents that the `lattice::utils::MacAddress` equality path is "strictly worse"
  than the `mac::eq()` free function it now delegates to (extra temporary-object copies), and
  `MacAddress::toString()` carries an explicit "no callers exist yet" comment. That's a
  self-documented partial-dead-code / superseded-idiom smell worth a deliberate decision
  (keep as the mesh's one MAC value type, or trim) rather than a silent skip.

All other sub-150-line files were skimmed (first ~15-25 lines and any suspicious neighbors,
e.g. the `network/` MAC-helper cluster and the `mesh/*Crypto*.h` cluster) and found to be
single-purpose, several explicitly documented as prior de-duplication work (post-Phase-G,
Phase H2). They're left unflagged; Tasks 2-4 remain free to flag any of them if a closer read
turns up something this pass missed.
