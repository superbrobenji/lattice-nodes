# Phase A — Persistence hardening (issue #43)

**Status:** Approved
**Date:** 2026-08-03
**Repo:** lattice-nodes
**Scope:** ESP32 firmware only. No wire-format changes. No cross-repo work.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase A)

## Context

Phase 0 (PR #58) rewrote `EepromManager` onto NVS `Preferences`. It preserved the pre-existing `saveBootEpoch` short-circuit behaviour under DEV_MODE and did not add write-return checking. Issue #43 is the residual gap:

1. **DEV_MODE never persists the epoch.** `saveBootEpoch` no-ops when `isDevMode` is true (`firmware/main/src/persistence/EepromManager.cpp:250-254`), so `loadBootEpoch()` returns the same value on every DEV boot. The AEAD nonce prefix `epoch(4) || seq(2) || origin_mac(6)` (see `firmware/main/src/mesh/E2ECrypto.h:100-116`) therefore repeats across DEV reboots, producing catastrophic ChaCha20-Poly1305 nonce reuse under the same key.
2. **NVS write returns are unchecked** everywhere. `Preferences::putUInt` / `putBytes` / `putBool` all return the number of bytes written; a silent 0-return in production leaves the epoch un-advanced and reproduces the DEV-mode failure mode in prod, undetected.

The umbrella spec locks Phase A's approach: RAM-only monotonically-increasing epoch seed in DEV_MODE + refuse-to-seal fallback + check every security-relevant NVS write return and escalate.

## Design

Three components. Each isolated. Each independently testable.

### 1. Boot-epoch source (`EepromManager`)

**New private state:**
- `uint32_t _devEpoch` — RAM-only DEV-mode epoch seed. Init 0.

**Behaviour changes:**
- `loadBootEpoch()`:
  - DEV: `return _devEpoch;`
  - Prod: unchanged (`return _prefs.getUInt(NVS_KEYS::BOOT_EPOCH, 0);`).
- `saveBootEpoch(uint32_t epoch)`:
  - DEV: `_devEpoch = epoch;` — no NVS write, RAM-only monotonic within a single power cycle.
  - Prod: `size_t n = _prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch); _persistOrEscalate(NVS_KEYS::BOOT_EPOCH, n, sizeof(uint32_t), /*securityRelevant=*/true);` — fatal on short write.

DEV mode's RAM-only epoch resets on every power cycle. That's acceptable because component §3 (seal-time rollback detection) refuses to seal against an epoch that would produce nonce reuse against the last-observed sealed epoch.

### 2. Tiered NVS write helper (`EepromManager`)

**New private method:**
```cpp
bool _persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant);
```

- `got == want` → log at DEBUG, return true.
- `got != want`:
  - `securityRelevant == true` → `lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, <unique code>, "NVS write failed: <key>")` — halts node with LED code. No return.
  - `securityRelevant == false` → `Logger::logln("NVS", "write failed key=... got=... want=...", LOG_ERROR)`, return false, caller decides.

**Applied at:**
- `saveBootEpoch` — securityRelevant=true
- `saveKnownMasterMac` — securityRelevant=true
- `saveKnownMasterMacSecondary` — securityRelevant=true (verify exists in current NVS API)
- `saveMeshKey` — securityRelevant=true if such setter exists (verify during impl; if not, note it as future addition)

**Non-security setters** (`saveMasterFlag`, `saveDevFlag`, `saveEnrolledFlag`, `saveAdapterType`, `saveNodeId`, `saveRebootCount`, `saveRebootReason`) get a lightweight wrap that logs an ERROR on short write but does not escalate — a stuck REBOOT_COUNT should not brick the device.

### 3. Seal-time rollback detection (`Mesh` + `E2ECrypto`)

**New state on `Mesh`:**
- `uint32_t _lastSealedEpoch` — init `UINT32_MAX` (sentinel: unset).
- `uint16_t _lastSealedSeq` — init 0.

**New private method:**
```cpp
void Mesh::_checkEpochRollback(uint32_t epoch, uint16_t seq);
```

- First call (`_lastSealedEpoch == UINT32_MAX`): snapshot `epoch` + `seq`. Return.
- Subsequent calls:
  - `epoch > _lastSealedEpoch` → update snapshot, return.
  - `epoch == _lastSealedEpoch && seq > _lastSealedSeq` → update seq, return.
  - Otherwise (would produce nonce reuse) → `lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO, lattice::core::ModuleDigit::MESH, <unique code>, "AEAD epoch rollback detected — refusing seal")` — halts node.

**Wiring:**
- `E2ECrypto::seal(...)` is called from `Mesh::sealAndSend` (or the equivalent transmit path — verify exact call site during impl).
- Insert `_checkEpochRollback(msg.epoch_num, msg.seq_num)` immediately before the ChaCha20-Poly1305 encrypt call.

### 4. Error-code additions (`ErrorCodes.h`)

Two new codes (assign next-free integers in the existing `MEMORY::EEPROM` and `CRYPTO::MESH` spaces during impl):
- `NVS_WRITE_FAILED_SECURITY` (or reuse existing MEMORY::EEPROM code with distinct minor)
- `AEAD_EPOCH_ROLLBACK`

## Data flow

```
setup()
  ├─ EepromManager::init()                        // NVS namespace open
  └─ Mesh::init()
       ├─ epoch = loadBootEpoch() + 1;            // DEV: from _devEpoch (0→1); prod: from NVS
       └─ saveBootEpoch(epoch);                    // DEV: to _devEpoch;
                                                   // prod: NVS + fatal-on-short-write

loop() / Mesh::sealAndSend(msg)
  ├─ _checkEpochRollback(msg.epoch_num, msg.seq_num);   // fatal if would reuse
  └─ E2ECrypto::seal(msg);                        // ChaCha20-Poly1305 encrypt
```

## Error handling

- **Fatal path**: `lattice::err::fail` triggers LED blink pattern (existing `ErrorCore` machinery) and 7-seg display code on TM1637-equipped nodes. No E2E frames leave the node afterwards; watchdog eventually resets, and on next boot the same check re-triggers if state is unrecoverable — operator must reflash.
- **Warn path**: `Logger::logln(..., LOG_ERROR)` — surfaces on serial console and reaches the hub aggregator via the serial adapter's health-report path.

## Testing

**Unit** (`tests/unit/test_eeprom_manager.cpp`):
- `SaveBootEpoch_ProdMode_Persists` (existing shape; assert `getUInt` returns saved value).
- `SaveBootEpoch_DevMode_UsesRAMSeed` — set devMode true, save 5, load returns 5; re-init instance (simulated reboot), load returns 0. Confirms RAM-only behaviour.
- `SaveBootEpoch_ProdMode_ShortWrite_Fatal` — mock `Preferences::putUInt` returning 0, assert `lattice::err::fail` invoked. Requires a test-hookable err mock (`tests/mocks/lattice_err_mock.h` — add if absent).
- `SaveKnownMasterMac_ShortWrite_Fatal` — same shape.
- `_persistOrEscalate_NonSecurity_WarnsAndReturnsFalse` — direct call, no fatal.

**Unit** (`tests/unit/test_mesh_logic.cpp` or new `test_epoch_rollback.cpp`):
- `CheckEpochRollback_FirstCall_Snapshots` — passes, `_lastSealedEpoch` set.
- `CheckEpochRollback_HigherEpoch_Passes`.
- `CheckEpochRollback_SameEpochHigherSeq_Passes`.
- `CheckEpochRollback_LowerEpoch_Fatal` — mock err, assert fatal.
- `CheckEpochRollback_SameEpochLowerSeq_Fatal`.
- `CheckEpochRollback_SameEpochSameSeq_Fatal`.

**E2E** (`tests/e2e/scenarios/test_e2e_aead.cpp`): existing suite should pass unchanged — happy-path sealing must still work. If any test relied on the DEV_MODE no-op behaviour of `saveBootEpoch`, update the test's expectations, do not weaken the fix.

**Manual verification**: after implementation, boot a DEV-mode master, seal a frame, hard-reset the node. Confirm that on second boot the seal path halts via `_checkEpochRollback` fatal (the last-sealed epoch state does not persist across reboot, but the epoch rollback still triggers when a fresh seal at epoch=1 tries after a previously-observed higher epoch — verified via injected fault or accepting that a fresh flash clears the guard, which is the documented "reflash to recover" path).

## Non-goals

- No wire-format changes. Nonce format `epoch(4) || seq(2) || origin_mac(6)` is unchanged.
- No cross-repo work. Purely nodes-side firmware.
- No dashboard / hub UI surfacing of the new error codes. Existing serial-adapter health path already carries error events.
- Not fixing #47 (hygiene) items that overlap with commit-check patterns — Phase E owns those.

## Files touched (estimate)

- `firmware/main/src/persistence/EepromManager.{h,cpp}` — new `_devEpoch`, `_persistOrEscalate`, updated 4 setters.
- `firmware/main/src/mesh/Mesh.{h,cpp}` — new `_lastSealedEpoch`/`_lastSealedSeq`, new `_checkEpochRollback`, one call-site insertion.
- `firmware/main/src/error/ErrorCodes.h` — 2 new codes.
- `tests/unit/test_eeprom_manager.cpp` — ~5 new cases.
- `tests/unit/test_mesh_logic.cpp` (or new `test_epoch_rollback.cpp`) — ~6 new cases.
- `tests/mocks/` — possibly a new `lattice_err_mock.{h,cpp}` if not present.

Rough size: ~200 LOC production + ~250 LOC test.
