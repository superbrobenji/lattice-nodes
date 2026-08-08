# Phase C — Repo-Wide Sweep Design

**Status:** Approved, ready for writing-plans.
**Date:** 2026-08-08
**Parent:** `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (Phase C)
**Source:** `docs/superpowers/specs/2026-08-07-clean-code-audit-findings.md` — 12 findings bucketed
Phase C (IDs 3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18). Phase B (mesh subsystem, PR #97) and
Phase E's spec entry (PR #98) are both merged to `main`; this design assumes that end state.

## Global Constraints

Inherited from the umbrella spec, unchanged: firmware-only, no wire-protocol changes, no
backwards-compatibility shims, "encapsulation yes, inheritance sparingly", library-replacement
caution (measure before adopting — see the Phase J libsodium-revert lesson), Tiger Style
preserved, full regression suite + CI flash/RAM size-delta check per phase.

New for this phase:

- **Single responsibility per file.** Every file this phase creates or splits must have one clear
  reason to change. This is the explicit design driver for the `EepromManager` split (below) and
  the standard every task's file layout is held to — a task that leaves a file doing two unrelated
  jobs has not met its brief.
- **Logger's UART migration ships as its own PR with its own before/after flash/RAM measurement**,
  reported separately from the rest of Phase C's size-neutral changes (finding 17's audit mandate).
  The other 5 work areas are readability-only and must show no net size delta.
- **Finding 11's `LED_ADAPTER` removal is verified safe without cross-repo coordination.**
  `adapter_types` (`Adapter.h:23`) is an unscoped `enum adapter_types : int32_t` with explicit
  values (`LED_ADAPTER = 3`), so deleting the enumerator does not renumber `UNKNOWN_ADAPTER`/
  `SERIAL_ADAPTER`/`PIR_ADAPTER`. `AdapterFactory::createAdapter()`'s switch never handles it
  (falls to `default:`), so nodes never transmits value 3 over the wire regardless of what
  `lattice-protocol` defines. No wire behavior changes; no protocol-repo edit needed.

## Work Areas

### 1. `main.cpp` boot decomposition (findings 3, 18)

**Files:** `firmware/main/main.cpp` (579 lines) — modified in place, no new files. `app_main()`'s
one 345-line, 15+-job function becomes a sequence of static functions, each one boot concern:

- `initDrivers()` — NVS partition init/erase-retry, ISR service install, UART driver install,
  `initArduino()`/`Serial.begin()` (until work area 4 removes these — see Sequencing)
- `initHardwareOutputs()` — the 3 `gpio_config_t` pin-group bring-ups, LED/button/7-seg/EEPROM
  hardware init, including a named `haltOnRedLedFailure(Led&, Led&)` helper for the inlined
  failure loop (`main.cpp:344-367`) — kept as boot-sequencing logic, not moved elsewhere, since the
  error-signaling itself depends on the LED that just failed
- `initSubsystems()` — dev-mode resolution, default-peer bootstrap, adapter creation+init, mesh
  init, master-role resolution, transmit-fn/callback wiring, provisioning pubkey print
- `spawnTasks()` — mesh-drain-task creation + notify-handle wiring, WDT config, housekeeping-task
  creation, PM config

`app_main()` itself becomes a 4-line call sequence. Finding 18 folds in here: a one-line comment
at the early unchecked `lattice::eeprom::init()` call site noting the later checked call is
authoritative (or restructure to a single call if `BootManager::check()`'s ordering allows it
cleanly — implementer's judgment during the task).

### 2. `main.cpp` enrollment-broadcast extraction (finding 14)

**Files:** `firmware/main/main.cpp`, `firmware/main/src/mesh/Mesh.h`, `firmware/main/src/mesh/Mesh.cpp`.

`housekeeping_task_fn`'s inline enrollment-broadcast state machine (`main.cpp:168-178` — its own
static interval tracking, `sendEnrollmentRequest()` call) moves to a new `Mesh::tickEnrollmentBroadcast()`
method, since `Mesh` already owns `isEnrolled()`/`sendEnrollmentRequest()` and the state the
10s-interval decision is based on. `housekeeping_task_fn` becomes pure delegation, matching the
rest of its 8 concerns.

### 3. `EepromManager` split into `persistence/eeprom/` (finding 4)

**Files:** new directory `firmware/main/src/persistence/eeprom/`, deletes
`firmware/main/src/persistence/EepromManager.{h,cpp}`. Flat `lattice::eeprom::` namespace
preserved throughout — zero function-signature changes, only file location and per-consumer
`#include` lines change.

- `EepromCore.{h,cpp}` — the generic typed-KV layer (`nvsGetU8`/`PutU8`/`GetU32`/`PutU32`/
  `GetBool`/`PutBool`/`GetBytes`/`PutBytes`/`Remove`/`HasKey`), `persistOrEscalate`, `crc16`,
  `detail::State`, `NVS_KEYS`, `EEPROM_SIZES`, `init()`, `setDevMode()`/`getDevMode()`,
  `clearAll()`, `dumpEEPROM()`, the `#ifdef UNIT_TEST` debug hooks. **Internal header** — the KV
  primitives are consumed only by the other `eeprom/*.cpp` files, never by code outside
  `persistence/eeprom/`.
- `EepromIdentity.{h,cpp}` — `loadKeypair`/`saveKeypair`, `loadNodeId`/`saveNodeId`
- `EepromRole.{h,cpp}` — `loadMasterFlag`/`saveMasterFlag`, `loadDevFlag`/`saveDevFlag`
- `EepromSecurity.{h,cpp}` — `loadMeshKey`/`saveMeshKey`, `loadKnownMasterMac`/`saveKnownMasterMac`/
  `clearKnownMasterMac`, `loadKnownMasterMacSecondary`/`saveKnownMasterMacSecondary`/
  `clearKnownMasterMacSecondary` (mesh key + TOFU trust anchors — both security/trust material)
- `EepromPeers.{h,cpp}` — `loadPeerList`/`savePeerList`/`hasPeers`/`clearPeerList`,
  `loadPeerRecord`/`savePeerRecord`/`erasePeerRecord`
- `EepromDiagnostics.{h,cpp}` — `loadRebootCount`/`saveRebootCount`/`saveRebootReason`/
  `loadRebootReason`, `loadBootEpoch`/`saveBootEpoch` (boot-time bookkeeping — reboot tracking and
  anti-replay epoch are both read/written on the same boot path)
- `EepromEnrollment.{h,cpp}` — `loadEnrolledFlag`/`saveEnrolledFlag`
- `EepromDeviceConfig.{h,cpp}` — `loadAdapterType`/`saveAdapterType`, `loadTxPowerPreset`/
  `saveTxPowerPreset`

**No facade re-export header.** This is the actual fix for finding 4's "any consumer can reach any
persistence function" problem, not just a file-size split — `Mesh.cpp` currently includes 1 header
and can call all 41 functions; after this change it includes only `EepromSecurity.h` +
`EepromDiagnostics.h` (its real dependencies) and the other 6 domain headers are simply not
visible to it. Each consumer's `#include "persistence/EepromManager.h"` is replaced with the
specific domain headers it actually calls into — grep each call site's `eeprom::` usage to
determine the set per consumer (`Mesh.cpp`, `Enrollment.cpp`, `PeerRegistry.cpp`, `main.cpp`, and
the test files).

### 4. Logger native UART migration (finding 17)

**Files:** `firmware/main/src/logging/Logger.h`, `firmware/main/src/logging/Logger.cpp`,
`firmware/main/main.cpp`, `firmware/main/CMakeLists.txt` (component dependency).

Rewrite `Logger.cpp`'s print path from `Serial.print`/`println`/`vprintf` onto `uart_write_bytes`
+ local `vsnprintf`, mirroring the pattern `SerialAdapter.cpp` already uses for its own
`UART_NUM_0` traffic. Drop `#include <Arduino.h>` from `Logger.h`. Remove `main.cpp`'s
`initArduino()`/`Serial.begin()` calls (Logger.h was the last consumer pulling in the
arduino-esp32 component — audit-verified via repo-wide grep, no other live Arduino/`WiFi.h`/
`esp32-hal` usage outside comments). Drop the arduino-esp32 component dependency from the build.

Ships as its own standalone PR, sequenced after work areas 1/2 (below) so it isn't fighting
`main.cpp` edits mid-flight. Report its own before/after flash+DRAM measurement in the PR
description — do not fold into any other work area's size accounting.

### 5. `ButtonHandler` dedup (finding 13)

**Files:** `firmware/main/src/app/ButtonHandler.h`.

Extract `static bool detectHold(Button& btn, uint64_t holdMs, bool& wasPressed, uint64_t& holdStart)`,
shared by `tickConfig()`/`tickReset()`, leaving only each function's differing post-hold action
(role toggle vs. arm/confirm wipe).

### 6. Trivial batch (findings 7, 8, 9, 10, 11, 12)

**Files:** `firmware/main/src/network/MacAddress.h` (delete), `firmware/main/src/mesh/Mesh.cpp`
(remove dead include + stale comment), `firmware/main/src/mesh/PeerRegistry.h` (remove dead
include), `firmware/main/src/error/Error.h` (remove legacy overload + `toDigit()`),
`firmware/main/main.cpp` (migrate its 2 legacy `err::fail` call sites), `firmware/main/src/hardware/input/Button.cpp`
(delegate to `GpioInput::init()`), `firmware/main/src/adapter/Adapter.cpp` (delete dead
pure-virtual body), `firmware/main/src/adapter/Adapter.h` (delete `LED_ADAPTER` enumerator),
`firmware/main/src/hardware/input/GpioInput.h`, `firmware/main/src/hardware/output/GpioOutput.h`
(drop `virtual` from destructors).

One task, six independent mechanical fixes — each is a deletion or a few-line change in a file
unrelated to the other five; none earns its own review cycle.

## Sequencing

```
main.cpp cluster (sequential, same file):
  1 (boot decomposition) -> 2 (enrollment-broadcast extraction) -> 4 (Logger, touches main.cpp last)

Parallel to the main.cpp cluster (worktree-safe, disjoint files):
  3 (EepromManager split)   -- touches persistence/eeprom/ + one #include line each in
                                Mesh.cpp/Enrollment.cpp/PeerRegistry.cpp/main.cpp (low conflict risk,
                                coordinate the main.cpp include-line touch with the cluster above)
  5 (ButtonHandler dedup)   -- fully isolated
  6 (trivial batch)         -- fully isolated, zero main.cpp overlap
```

Work area 4 (Logger) is last in the main.cpp cluster because it's the highest-risk, standalone-PR
item — landing boot decomposition and enrollment extraction first means Logger's migration doesn't
have to account for `main.cpp` changing under it mid-review.

## Testing

Same discipline as Phase B: full unit + e2e suite after every task, no direct test coverage gaps
shipped to review (learned from Phase B Round 1 Task 6). `EepromManager`'s split (work area 3) is
the highest test-surface-area change — the e2e harness's `NodeContext.cpp` snapshot/restore
depends on `detail::State` staying a flat POD in `EepromCore.h`; confirm the harness still builds
and passes before considering that task done.
