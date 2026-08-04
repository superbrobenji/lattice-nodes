# Phase D — Enrollment master-pubkey pinning (#42)

**Status:** Approved
**Date:** 2026-08-04
**Repo:** lattice-nodes
**Scope:** ESP32 firmware + a small provisioning helper. No wire-format changes. No cross-repo work.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase D).
**Depends on:** Phase H (hub durable master keypair) — already shipped.

## Context

A node authenticates its master purely by trust-on-first-use. First JOIN_ACK / master beacon pins the origin MAC as the master; no cryptographic proof the responder is the legitimate master. An RF-present attacker at the enrollment instant can MITM: they become the node's trusted master, own the E2E key, and read/inject that node's traffic. Post-Phase-H the JOIN_ACK carries the master's stable pubkey (`enrollment_public_key`), but under plain TOFU that key is unauthenticated on the wire.

Fix: pin the master pubkey at flash time and compare against it in the enrollment path. Rejects any wrong key regardless of timing (real authentication, not window-narrowing). Cheap — one 32-byte constant per deployment, shared across all nodes. Dual-master (#88) trust is transitive: pin the primary's pubkey → primary's JOIN_ACK is authenticated → secondary pubkey rides inside that authenticated frame → trusted without a second pin.

## Design

### 1. Provisioning — gitignored per-deployment header

Three files added; one gitignored:

**`firmware/main/config/master_pubkey_pin.h.example`** (committed, placeholder values):
```cpp
#pragma once
#include <cstdint>
namespace lattice { namespace mesh { namespace pin {
// PLACEHOLDER — replace with real values from the deployment's master keypair.
// Regenerate this file via `tools/gen_master_pubkey_pin.py <masterkey.json> <master-mac>`.
constexpr uint8_t MASTER_PUBKEY[32] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t MASTER_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
}}}
```

**`firmware/main/config/master_pubkey_pin.h`** (gitignored, per-deployment): identical structure with real values. Generated via the tool below.

**`firmware/main/config/master_pubkey_pin_wrapper.h`** (committed):
```cpp
#pragma once
// Force the deployer to generate a real pin header. DEV_MODE builds fall back
// to the placeholder so dev iteration works without a provisioning step.
#if __has_include("master_pubkey_pin.h")
  #include "master_pubkey_pin.h"
#else
  #if !defined(LATTICE_ALLOW_EXAMPLE_PIN)
    #error "firmware/main/config/master_pubkey_pin.h not found. Run tools/gen_master_pubkey_pin.py OR build with -DLATTICE_ALLOW_EXAMPLE_PIN=1 for DEV_MODE."
  #endif
  #include "master_pubkey_pin.h.example"
#endif
```

CMake surface: host-test target defines `LATTICE_ALLOW_EXAMPLE_PIN`; firmware build does not (deployer must generate the real header before firmware build succeeds).

`.gitignore` addition:
```
firmware/main/config/master_pubkey_pin.h
```

**`tools/gen_master_pubkey_pin.py`** (~50 LOC): reads the hub's `masterkey.json` (produced by Phase H's `LoadOrGenerateMasterKey`), takes a MAC string arg (e.g. `aa:bb:cc:dd:ee:ff`), writes the gitignored header with the correct byte arrays. Idempotent — overwrites in place.

### 2. JOIN_ACK verification (`Enrollment::processJoinAck`)

Insertion point: `firmware/main/src/mesh/Enrollment.cpp` at the current TOFU-learn branch (lines 99-133 area — verify exact location at impl time). Insert BEFORE the origin-MAC TOFU check (currently the first gate):

```cpp
#if !defined(LATTICE_TEST_PIN_OVERRIDE)
if (!lattice::config::DEV_MODE) {
  if (memcmp(msg.public_key, lattice::mesh::pin::MASTER_PUBKEY,
             sizeof(lattice::mesh::pin::MASTER_PUBKEY)) != 0) {
    Logger::logln("ENROLL", "JOIN_ACK master pubkey mismatch pin — drop",
                  LogLevel::LOG_ERROR);
    return;
  }
}
#endif
```

Strong authentication — attacker cannot forge the pubkey without the master's private key.

Note: `msg.public_key` is the master's pubkey (per Phase H server-side, hub sets this to `masterPublicKey`); this is exactly what the pin holds.

### 3. Beacon origin verification (`Mesh::processMasterBeacon`)

Insertion point: at the top of `processMasterBeacon`, before the current TOFU-learn branch. Same DEV_MODE bypass shape:

```cpp
#if !defined(LATTICE_TEST_PIN_OVERRIDE)
if (!lattice::config::DEV_MODE) {
  if (memcmp(msg.origin_mac_address, lattice::mesh::pin::MASTER_MAC,
             sizeof(lattice::mesh::pin::MASTER_MAC)) != 0) {
    Logger::logln("MESH", "Beacon origin MAC mismatch pin — drop",
                  LogLevel::LOG_ERROR);
    return;
  }
}
#endif
```

**Weaker guarantee than the JOIN_ACK pubkey pin.** WiFi MACs are trivially spoofable — a MAC-spoofing attacker who sets its WiFi to the pinned MAC still passes this check. Rejects naive attackers. Real beacon auth would require a signed field in the beacon (wire change, v5), explicitly out of scope for Phase D.

### 4. DEV_MODE

`lattice::config::DEV_MODE == true` (compile-time constant in `project_config.h`) bypasses both checks. Add a boot-time WARN log in `Mesh::init`:

```cpp
if (lattice::config::DEV_MODE) {
  Logger::logln("MESH", "DEV_MODE: master pubkey pin disabled — do not ship this build",
                LogLevel::LOG_WARN);
}
```

Rationale: `DEV_MODE` already short-circuits NVS + generates a fresh keypair per boot. Pin check in DEV would reject enrollment against a freshly-flashed dev master. Skip the check; make the state visible in logs.

### 5. Dual-master (#88) — transitive trust

Zero code change. Primary's JOIN_ACK is now authenticated by pubkey pin → the `secondary_master_mac` + `secondary_public_key` fields ride inside that authenticated frame → trusted. Secondary beacon-TOFU still uses `MASTER_MAC` pin, which does NOT distinguish primary vs. secondary — the pin's MAC array is the primary's. A secondary master would fail the beacon-MAC pin.

**Decision:** for Phase D, only the primary master's MAC is pinned. Dual-master beacon reception falls back to TOFU-after-JOIN_ACK (once the node has learned the secondary from the primary's JOIN_ACK, the secondary is trusted by that path). Post-Phase D, if secondary beacons matter, a separate pinned `SECONDARY_MASTER_MAC` can be added — trivial extension.

### 6. Error handling

- JOIN_ACK mismatch: `Logger::logln(..., LOG_ERROR)`, return. Node keeps scanning for a legitimate master (unchanged post-drop behaviour — matches current unknown-origin drop).
- Beacon mismatch: same.
- Missing `master_pubkey_pin.h` header at firmware-build time: compile-time `#error`. Cannot ship without provisioning.
- Missing header in host-tests: falls back to `.example` via `LATTICE_ALLOW_EXAMPLE_PIN` define set in the tests CMake target.

### 7. Testing

Test-only pin override header: `tests/mocks/master_pubkey_pin.h` — a fixed known value shared by all tests. Loaded via the same wrapper.

Unit (`tests/unit/test_enrollment.cpp` extend):
- `ProcessJoinAck_ValidPubkey_Enrolls` — DEV_MODE=false, msg.public_key == pin → node registers master + `hasMasterMac == true`.
- `ProcessJoinAck_WrongPubkey_DropsNoEnroll` — DEV_MODE=false, msg.public_key ≠ pin → node drops, `hasMasterMac == false`, no `saveKnownMasterMac` call.
- `ProcessJoinAck_DevMode_SkipsCheck` — flag DEV_MODE=true via test override, wrong pubkey → still enrols (with WARN in log).

Unit (`tests/unit/test_mesh_logic.cpp` extend):
- `ProcessMasterBeacon_ValidOriginMac_Learns` — origin == pin MAC → `enrollment.knownMasterMac` set.
- `ProcessMasterBeacon_WrongOriginMac_Drops` — origin ≠ pin → drops, `enrollment.hasMasterMac == false`.
- `ProcessMasterBeacon_DevMode_SkipsCheck` — DEV_MODE=true → learns regardless.

DEV_MODE toggling for tests: since `lattice::config::DEV_MODE` is `constexpr`, tests can't flip it at runtime. Two options:
- (a) Split the check body into an inline helper `enforcePinIfProduction(...)` that reads a static `bool _testPinBypass = false;` under `#ifdef UNIT_TEST`. Tests set the flag; production build hard-inlines the compile-time constant.
- (b) Use `#ifdef LATTICE_TEST_PIN_OVERRIDE` at the check site to bypass entirely; tests are then split into pin-active + pin-bypassed test binaries. Heavier.

Prefer (a) — smaller test surface. Verified during implementation.

E2E: existing enrollment scenarios in `tests/e2e/scenarios/test_enrollment_e2e.cpp` — extend the fixture to pre-populate the test-pin header with the sim master's actual pubkey so scenarios pass. No new scenarios needed; existing valid-enrollment paths continue to work.

### 8. Non-goals

- Beacon-level cryptographic authentication (wire change, v5).
- Master pubkey rotation (deployer reflashes the fleet).
- Server-side attestation (redundant per umbrella).
- Pinning the secondary master's MAC (deferred; transitive-trust suffices for Phase D).
- Not fixing #47 (hygiene) or Phase E items adjacent to touched code.

### 9. Files touched (estimate)

Committed:
- `firmware/main/config/master_pubkey_pin.h.example` (new)
- `firmware/main/config/master_pubkey_pin_wrapper.h` (new)
- `firmware/main/project_config.h` — no change expected; `DEV_MODE` already there.
- `firmware/main/src/mesh/Enrollment.cpp` (+~15 LOC pin check)
- `firmware/main/src/mesh/Mesh.cpp` (+~15 LOC beacon pin check + boot-time DEV_MODE warn)
- `.gitignore` (+1 line)
- `tools/gen_master_pubkey_pin.py` (new, ~50 LOC)
- `tests/mocks/master_pubkey_pin.h` (new)
- `tests/CMakeLists.txt` — add `LATTICE_ALLOW_EXAMPLE_PIN` compile flag; include `tests/mocks` before `firmware/main/config` so mock wins.
- `tests/unit/test_enrollment.cpp` — 3 new cases.
- `tests/unit/test_mesh_logic.cpp` — 3 new cases.
- `tests/e2e/scenarios/test_enrollment_e2e.cpp` — fixture may need pin-header seed helper.

Gitignored:
- `firmware/main/config/master_pubkey_pin.h`.

Rough size: ~150 LOC production + tooling, ~200 LOC tests.
