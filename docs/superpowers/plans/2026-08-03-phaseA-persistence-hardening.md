# Phase A — Persistence hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix AEAD nonce reuse in DEV_MODE builds and add security-tiered NVS write-return checks per Phase A of the umbrella spec (issue #43).

**Architecture:** Three isolated components inside `firmware/main/`: RAM-only monotonic epoch seed in `EepromManager` for DEV mode, a tiered `_persistOrEscalate` helper that halts the node on short writes to security-relevant NVS keys, and a seal-time rollback guard on `Mesh` that halts before ChaCha20-Poly1305 would produce nonce reuse. No wire-format changes; no cross-repo work.

**Tech Stack:** C++ (ESP-IDF w/ arduino-esp32 3.3.10), NVS via `Preferences`, mbedtls ChaCha20-Poly1305, GoogleTest+Ctest host unit suite, GoogleTest e2e harness in `tests/e2e/`.

## Global Constraints

- No wire-format changes. Nonce layout `epoch(4 LE) || seq(2 LE) || origin_mac(6)` is preserved verbatim.
- No cross-repo work. Purely `lattice-nodes` firmware. Do not touch `lattice-protocol` or `lattice-hub`.
- Tiger-Style — static alloc after `setup()`, no dynamic allocation on the hot path, WDT-aware.
- Error escalation uses `lattice::err::fail(ErrorTypeDigit, ModuleDigit, sub, msg)`. Under `UNIT_TEST`, `err::fail` throws `lattice::err::FatalError` and increments `lattice_test_errFailCount` — tests use `EXPECT_THROW(..., lattice::err::FatalError)` or check the counter.
- `Preferences::putUInt/putBytes/putBool` return the number of bytes written; `0` = failure.
- Error codes follow existing `makeErrorCode(TypeDigit, ModuleDigit, sub)` convention. Sub-codes are single-digit `0-9`.
- Design doc: `docs/superpowers/specs/2026-08-03-phaseA-persistence-hardening-design.md`.
- Parent spec: `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase A).

---

### Task 1: DEV-mode RAM-only epoch seed

**Files:**
- Modify: `firmware/main/src/persistence/EepromManager.h` — add `uint32_t _devEpoch = 0;` private member.
- Modify: `firmware/main/src/persistence/EepromManager.cpp` — rework `loadBootEpoch` + `saveBootEpoch` DEV path.
- Test: `tests/unit/test_eeprom_manager.cpp` — add 3 cases.

**Interfaces:**
- Consumes: existing `Preferences& _prefs`, `bool isDevMode`, `NVS_KEYS::BOOT_EPOCH`.
- Produces: unchanged public API — `uint32_t loadBootEpoch()` and `void saveBootEpoch(uint32_t)`. DEV callers now get a monotonic-within-power-cycle value; prod callers unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_eeprom_manager.cpp`:

```cpp
TEST_F(EEPROMMgrTest, SaveBootEpoch_DevMode_UsesRAMSeed) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(true);
  mgr.saveBootEpoch(5);
  EXPECT_EQ(mgr.loadBootEpoch(), 5u);
  mgr.saveBootEpoch(7);
  EXPECT_EQ(mgr.loadBootEpoch(), 7u);
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_DevMode_DoesNotTouchNVS) {
  auto& mgr = EepromManager::getInstance();
  Preferences::_store.clear();
  mgr.setDevMode(true);
  mgr.saveBootEpoch(42);
  // NVS store must be empty — DEV never persists.
  EXPECT_TRUE(Preferences::_store.empty());
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_Persists) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(false);
  mgr.saveBootEpoch(9);
  EXPECT_EQ(mgr.loadBootEpoch(), 9u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_eeprom_manager --parallel
ctest --test-dir tests/build -R EEPROMMgrTest.SaveBootEpoch --output-on-failure
```

Expected: `SaveBootEpoch_DevMode_UsesRAMSeed` FAIL — `loadBootEpoch()` still reads from NVS (`0` in DEV since Phase-0 short-circuited the save side). `SaveBootEpoch_DevMode_DoesNotTouchNVS` likely PASS by accident (current save short-circuits) — retain as regression guard. `SaveBootEpoch_ProdMode_Persists` PASS.

- [ ] **Step 3: Add `_devEpoch` field to header**

In `firmware/main/src/persistence/EepromManager.h`, inside the class, private section, adjacent to `bool isDevMode;`:

```cpp
uint32_t _devEpoch = 0;  // DEV_MODE RAM-only monotonic boot-epoch seed (issue #43)
```

- [ ] **Step 4: Rework loadBootEpoch + saveBootEpoch**

Replace lines 244-254 in `firmware/main/src/persistence/EepromManager.cpp`:

```cpp
uint32_t EepromManager::loadBootEpoch() {
  if (!ensureInitialized())
    return 0;
  if (isDevMode)
    return _devEpoch;
  return _prefs.getUInt(NVS_KEYS::BOOT_EPOCH, 0);
}

void EepromManager::saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    _devEpoch = epoch;
    return;
  }
  _prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch);
}
```

Note: prod-mode write-return check comes in Task 2. Keep the bare `putUInt` for now.

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_eeprom_manager --parallel
ctest --test-dir tests/build -R EEPROMMgrTest.SaveBootEpoch --output-on-failure
```

Expected: all 3 PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/persistence/EepromManager.h \
        firmware/main/src/persistence/EepromManager.cpp \
        tests/unit/test_eeprom_manager.cpp
git commit -m "feat(persistence): DEV-mode RAM-only boot-epoch seed

DEV_MODE previously short-circuited saveBootEpoch, so loadBootEpoch
returned the same value every DEV boot and the AEAD nonce prefix
epoch(4)||seq(2)||origin_mac(6) repeated across reboots. Adds a
private _devEpoch that DEV-mode load/save use in place of NVS,
giving dev builds a monotonic epoch within a single power cycle.
Prod path unchanged pending Task 2.

Part of Phase A (issue #43)."
```

---

### Task 2: Tiered NVS write-return helper + security-relevant setter wiring

**Files:**
- Modify: `firmware/main/src/persistence/EepromManager.h` — declare private `_persistOrEscalate`.
- Modify: `firmware/main/src/persistence/EepromManager.cpp` — implement helper; wire `saveBootEpoch`, `saveKnownMasterMac`, `saveKnownMasterMacSecondary` (if present), `saveMeshKey` (if present).
- Modify: `firmware/main/src/error/ErrorCodes.h` — no additions needed; reuse existing `MEMORY::EEPROM` type/module (sub-codes 5-8 for new failures, if 0-4 already used verify during impl).
- Test: `tests/unit/test_eeprom_manager.cpp` — add 4 cases.

**Interfaces:**
- Consumes: `lattice::err::fail(ErrorTypeDigit, ModuleDigit, sub, msg)` from `src/error/Error.h`. Under `UNIT_TEST` this throws `lattice::err::FatalError`.
- Consumes: `Preferences::_store` — the map exposed by the mock in `tests/mocks/Preferences.{h,cpp}` for direct tampering.
- Produces: private `bool _persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant)`. Not part of public API; other tasks do not depend on its signature.

- [ ] **Step 1: Verify current setter surface**

Before writing tests, list every `EepromManager::save*` that writes NVS bytes (grep `_prefs.put`). Confirm which of these persist security-relevant material:
- `saveBootEpoch` (BOOT_EPOCH) — security-relevant
- `saveKnownMasterMac` (KNOWN_MASTER_MAC) — security-relevant
- `saveKnownMasterMacSecondary` (KNOWN_MASTER_MAC_SECONDARY) — security-relevant if present
- `saveMeshKey` (MESH_KEY) — security-relevant if present
- Everything else (`saveMasterFlag`, `saveDevFlag`, `saveEnrolledFlag`, `saveAdapterType`, `saveNodeId`, `saveRebootCount`, `saveRebootReason`) — non-security

Cache the list; the wiring in Step 4 must cover exactly the security-relevant setters and no more.

- [ ] **Step 2: Write the failing tests**

Add to `tests/unit/test_eeprom_manager.cpp`:

```cpp
#include "error/Error.h"

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_ShortWrite_Fatal) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(false);
  Preferences::_shortWriteKey = NVS_KEYS::BOOT_EPOCH;   // mock: next putUInt for this key returns 0
  EXPECT_THROW(mgr.saveBootEpoch(1), lattice::err::FatalError);
  Preferences::_shortWriteKey = nullptr;
}

TEST_F(EEPROMMgrTest, SaveKnownMasterMac_ShortWrite_Fatal) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(false);
  uint8_t mac[6] = {1,2,3,4,5,6};
  Preferences::_shortWriteKey = NVS_KEYS::KNOWN_MASTER_MAC;
  EXPECT_THROW(mgr.saveKnownMasterMac(mac), lattice::err::FatalError);
  Preferences::_shortWriteKey = nullptr;
}

TEST_F(EEPROMMgrTest, SaveBootEpoch_ProdMode_FullWrite_NoFatal) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(false);
  int before = lattice_test_errFailCount;
  mgr.saveBootEpoch(3);
  EXPECT_EQ(lattice_test_errFailCount, before);
  EXPECT_EQ(mgr.loadBootEpoch(), 3u);
}

TEST_F(EEPROMMgrTest, SaveRebootCount_ShortWrite_WarnsNoFatal) {
  auto& mgr = EepromManager::getInstance();
  mgr.setDevMode(false);
  int before = lattice_test_errFailCount;
  Preferences::_shortWriteKey = NVS_KEYS::REBOOT_COUNT;
  mgr.saveRebootCount(1);                 // Non-security setter: must NOT escalate
  EXPECT_EQ(lattice_test_errFailCount, before);
  Preferences::_shortWriteKey = nullptr;
}
```

- [ ] **Step 3: Extend the Preferences mock to support short-write injection**

Add to `tests/mocks/Preferences.h` inside `class Preferences`:

```cpp
static const char* _shortWriteKey;   // when non-null, next put*(key,...) returns 0
```

Add to `tests/mocks/Preferences.cpp`:

```cpp
const char* Preferences::_shortWriteKey = nullptr;
```

In every `putUInt`/`putBytes`/`putBool` mock body, at the top:

```cpp
if (_shortWriteKey && std::strcmp(key, _shortWriteKey) == 0) {
    return 0;   // simulate short write
}
```

- [ ] **Step 4: Declare and implement `_persistOrEscalate`**

In `EepromManager.h` private section:

```cpp
bool _persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant);
```

In `EepromManager.cpp`, add near the bottom of the file (before the closing namespace):

```cpp
bool EepromManager::_persistOrEscalate(const char* key, size_t got, size_t want,
                                       bool securityRelevant) {
  if (got == want) {
    return true;
  }
  if (securityRelevant) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY,
                       lattice::core::ModuleDigit::EEPROM, 5,
                       "NVS write failed (security-relevant key)");
    return false;  // unreachable outside UNIT_TEST
  }
  Logger::logln("NVS", String("write failed key=") + key +
                          " got=" + String((unsigned)got) +
                          " want=" + String((unsigned)want),
                LogLevel::LOG_ERROR);
  return false;
}
```

Rework the four security-relevant setters. `saveBootEpoch` becomes:

```cpp
void EepromManager::saveBootEpoch(uint32_t epoch) {
  if (!ensureInitialized())
    return;
  if (isDevMode) {
    _devEpoch = epoch;
    return;
  }
  size_t n = _prefs.putUInt(NVS_KEYS::BOOT_EPOCH, epoch);
  _persistOrEscalate(NVS_KEYS::BOOT_EPOCH, n, sizeof(uint32_t), /*securityRelevant=*/true);
}
```

`saveKnownMasterMac` becomes:

```cpp
void EepromManager::saveKnownMasterMac(const uint8_t* mac) {
  if (!ensureInitialized() || isDevMode)
    return;
  size_t n = _prefs.putBytes(NVS_KEYS::KNOWN_MASTER_MAC, mac, 6);
  _persistOrEscalate(NVS_KEYS::KNOWN_MASTER_MAC, n, 6, /*securityRelevant=*/true);
  logOperation("Known master MAC saved");
}
```

Apply the same pattern to `saveKnownMasterMacSecondary` and `saveMeshKey` if they exist (verify against Step-1 list). Non-security setters left unchanged.

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_eeprom_manager --parallel
ctest --test-dir tests/build -R EEPROMMgrTest.Save --output-on-failure
```

Expected: 4 new tests PASS, all pre-existing PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/persistence/EepromManager.h \
        firmware/main/src/persistence/EepromManager.cpp \
        tests/mocks/Preferences.h \
        tests/mocks/Preferences.cpp \
        tests/unit/test_eeprom_manager.cpp
git commit -m "feat(persistence): tiered NVS write-return escalation

Adds _persistOrEscalate helper. Security-relevant setters
(saveBootEpoch, saveKnownMasterMac[+Secondary], saveMeshKey) escalate
via lattice::err::fail on short write — a silent NVS failure here is
a security-relevant event that would silently degrade AEAD nonce
uniqueness or master identity. Non-security setters log ERROR and
continue.

Part of Phase A (issue #43)."
```

---

### Task 3: Seal-time epoch-rollback detection on Mesh

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h` — add `_lastSealedEpoch`, `_lastSealedSeq` private members; declare `_checkEpochRollback`.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — implement `_checkEpochRollback`; call it before both `sealPayload` sites (currently at Mesh.cpp:454 and Mesh.cpp:592 — verify line numbers at implementation time).
- Test: `tests/unit/test_mesh_logic.cpp` — add 6 cases in a new fixture `MeshEpochRollbackTest`.

**Interfaces:**
- Consumes: `lattice::err::fail(CRYPTO=..., MESH, sub, msg)` — CRYPTO is not currently in the `ErrorTypeDigit` enum. During Step 1, either (a) add `CRYPTO = 7` to `ErrorCodes.h`, or (b) reuse `MEMORY` (semantically inaccurate but fits existing enum). Prefer (a): CRYPTO more accurately types the failure and the 3-digit code composes correctly on 7-seg.
- Consumes: `mesh_message.epoch_num`, `mesh_message.seq_num`.
- Produces: private `void Mesh::_checkEpochRollback(uint32_t epoch, uint16_t seq)`. On rollback, calls `err::fail` and returns (throws under UNIT_TEST). No return value — a rollback should never reach the encryption call.

- [ ] **Step 1: Add CRYPTO error type**

In `firmware/main/src/error/ErrorCodes.h`, extend the enum:

```cpp
enum class ErrorTypeDigit : uint8_t {
  GENERIC = 1,
  SENSOR = 2,
  COMM = 3,
  MEMORY = 4,
  HARDWARE = 5,
  CONFIG = 6,
  CRYPTO = 7,   // AEAD/ECDH failures — Phase A adds AEAD_EPOCH_ROLLBACK sub=1
};
```

- [ ] **Step 2: Write the failing tests**

Create or append to `tests/unit/test_mesh_logic.cpp`:

```cpp
#include "error/Error.h"
#include "mesh/Mesh.h"

class MeshEpochRollbackTest : public ::testing::Test {
protected:
  lattice::mesh::Mesh mesh;
  void SetUp() override { /* mesh default-constructed; _lastSealedEpoch = UINT32_MAX */ }
};

TEST_F(MeshEpochRollbackTest, FirstCall_Snapshots) {
  EXPECT_NO_THROW(mesh._checkEpochRollback(3, 7));
}

TEST_F(MeshEpochRollbackTest, HigherEpoch_Passes) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_NO_THROW(mesh._checkEpochRollback(4, 0));
}

TEST_F(MeshEpochRollbackTest, SameEpochHigherSeq_Passes) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_NO_THROW(mesh._checkEpochRollback(3, 8));
}

TEST_F(MeshEpochRollbackTest, LowerEpoch_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(2, 0), lattice::err::FatalError);
}

TEST_F(MeshEpochRollbackTest, SameEpochLowerSeq_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(3, 6), lattice::err::FatalError);
}

TEST_F(MeshEpochRollbackTest, SameEpochSameSeq_Fatal) {
  mesh._checkEpochRollback(3, 7);
  EXPECT_THROW(mesh._checkEpochRollback(3, 7), lattice::err::FatalError);
}
```

To let tests reach a private method, either (a) declare `MeshEpochRollbackTest` a `friend class` inside `Mesh`, or (b) expose a thin public `checkEpochRollbackForTest(uint32_t, uint16_t)` that just calls the private. Prefer (a) — no production surface added.

Add inside `class Mesh { ... }` private section:

```cpp
friend class MeshEpochRollbackTest;
```

- [ ] **Step 3: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R MeshEpochRollbackTest --output-on-failure
```

Expected: compile error — `_checkEpochRollback` not declared.

- [ ] **Step 4: Add state + declare + implement `_checkEpochRollback`**

In `firmware/main/src/mesh/Mesh.h`, inside `class Mesh` private section:

```cpp
uint32_t _lastSealedEpoch = UINT32_MAX;   // sentinel: no seal observed yet
uint16_t _lastSealedSeq   = 0;
void _checkEpochRollback(uint32_t epoch, uint16_t seq);
```

In `firmware/main/src/mesh/Mesh.cpp`, add near the top of the file (after includes, before the namespace body — or in the appropriate private-methods section per file style):

```cpp
void Mesh::_checkEpochRollback(uint32_t epoch, uint16_t seq) {
  if (_lastSealedEpoch == UINT32_MAX) {
    _lastSealedEpoch = epoch;
    _lastSealedSeq   = seq;
    return;
  }
  if (epoch > _lastSealedEpoch) {
    _lastSealedEpoch = epoch;
    _lastSealedSeq   = seq;
    return;
  }
  if (epoch == _lastSealedEpoch && seq > _lastSealedSeq) {
    _lastSealedSeq = seq;
    return;
  }
  lattice::err::fail(lattice::core::ErrorTypeDigit::CRYPTO,
                     lattice::core::ModuleDigit::MESH, 1,
                     "AEAD epoch rollback — refusing seal");
}
```

- [ ] **Step 5: Wire the check at both seal call-sites**

At `firmware/main/src/mesh/Mesh.cpp:454` (the master-uplink `sealPayload` call inside the master-only branch), directly above the `if (!masterE2EKeys(...) || !sealPayload(kUp, msg))` line, insert:

```cpp
    _checkEpochRollback(msg.epoch_num, msg.seq_num);
```

At `firmware/main/src/mesh/Mesh.cpp:592` (the peer-downlink `sealPayload` call), same insertion above the `if (!peerE2EKeys(destMac, ...) || !sealPayload(kDown, msg))` line:

```cpp
  _checkEpochRollback(msg.epoch_num, msg.seq_num);
```

Verify line numbers before editing — the file has churned recently. Use `grep -n "sealPayload" firmware/main/src/mesh/Mesh.cpp` to find the exact locations.

- [ ] **Step 6: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R MeshEpochRollbackTest --output-on-failure
```

Expected: 6 PASS.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/src/error/ErrorCodes.h \
        firmware/main/src/mesh/Mesh.h \
        firmware/main/src/mesh/Mesh.cpp \
        tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): seal-time AEAD epoch-rollback guard

Mesh now tracks the (epoch, seq) of the last sealed frame. Before
each sealPayload call, _checkEpochRollback verifies the new
(epoch, seq) strictly advances; otherwise it halts the node via
lattice::err::fail(CRYPTO, MESH, 1). Combined with the DEV-mode RAM
epoch seed (Task 1) this refuses to reuse an AEAD nonce prefix under
one key even when the persisted epoch fails to advance.

Adds ErrorTypeDigit::CRYPTO = 7 for AEAD/ECDH failures.

Part of Phase A (issue #43)."
```

---

### Task 4: E2E verification + close the issue

**Files:**
- No production changes. Run existing e2e suites; add nothing new.
- Modify: `.superpowers/sdd/phaseA-persistence-hardening/progress.md` — new SDD ledger file (gitignored).

**Interfaces:**
- Consumes: unit + e2e test binaries built by CMake.
- Produces: nothing consumed by future tasks; this is the exit-gate task.

- [ ] **Step 1: Full host suite green**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

Expected: 0 failures. Pre-existing 2 DualMasterTest failures (documented in Phase 0 report) are e2e-only; still excluded here.

- [ ] **Step 2: E2E suite green (AEAD path unchanged)**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 -L e2e -R "aead|enroll"
```

Expected: `test_e2e_aead` and enrollment scenarios all PASS. If a scenario relied on DEV_MODE `saveBootEpoch` no-op behaviour, adjust the scenario's assertion (do not weaken the production fix).

- [ ] **Step 3: Write SDD progress ledger**

Create `.superpowers/sdd/phaseA-persistence-hardening/progress.md`:

```markdown
# SDD ledger — plan: docs/superpowers/plans/2026-08-03-phaseA-persistence-hardening.md
Task 1: complete (commit <sha>) — DEV RAM epoch seed
Task 2: complete (commit <sha>) — tiered NVS write escalation
Task 3: complete (commit <sha>) — seal-time rollback guard
Task 4: complete — full suite green, e2e AEAD path green
```

- [ ] **Step 4: Push branch + open PR**

```bash
git push -u origin feat/phaseA-persistence-hardening
gh pr create --title "feat(phaseA): persistence hardening (closes #43)" \
             --body "$(cat <<'EOF'
Implements docs/superpowers/plans/2026-08-03-phaseA-persistence-hardening.md.

Closes #43.

## Summary
- DEV-mode RAM-only boot-epoch seed (no more nonce reuse across dev reboots).
- Tiered NVS write-return escalation (security-relevant writes fatal).
- Seal-time AEAD epoch-rollback guard on Mesh.
- No wire-format changes, no cross-repo work.

## Test plan
- [x] Host unit suite green.
- [x] E2E AEAD scenarios green.
- [ ] CI green.
EOF
)"
```

---

## Self-review

**Spec coverage:**
- §Design/1 (Boot-epoch source) → Task 1.
- §Design/2 (Tiered NVS write helper) → Task 2.
- §Design/3 (Seal-time rollback detection) → Task 3.
- §Design/4 (Error-code additions) → folded into Task 3 (CRYPTO=7 added at ErrorCodes.h).
- §Testing (unit + e2e) → distributed across Task 1/2/3 for unit, Task 4 for e2e regression.
- §Non-goals → respected: no wire changes, no cross-repo, no #47 overlap.

**Type consistency:**
- `_devEpoch : uint32_t` — declared Task 1 Step 3, consumed Task 1 Step 4.
- `_persistOrEscalate(const char*, size_t, size_t, bool) : bool` — declared and consumed within Task 2.
- `_checkEpochRollback(uint32_t, uint16_t)` — declared Task 3 Step 4, consumed Task 3 Step 5.
- `_lastSealedEpoch : uint32_t = UINT32_MAX`, `_lastSealedSeq : uint16_t` — declared and consumed within Task 3.
- `ErrorTypeDigit::CRYPTO = 7` — added Task 3 Step 1, consumed Task 3 Step 4.
- `Preferences::_shortWriteKey : const char*` — added Task 2 Step 3, consumed Task 2 Steps 2 + 4.
- `lattice_test_errFailCount` and `lattice::err::FatalError` — pre-existing, verified in `firmware/main/src/error/Error.h`.

**Placeholder scan:** none. Every step has concrete code or exact command lines. The one hedge — "verify line numbers before editing" in Task 3 Step 5 — is legitimate (the file churns) and accompanied by the exact grep command to resolve it.

**Scope check:** four tasks, all in one repo, one design doc, single PR — appropriate for one plan.
