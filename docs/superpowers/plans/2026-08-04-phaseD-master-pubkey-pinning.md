# Phase D — Master-pubkey pinning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Pin the master public key at flash time and verify JOIN_ACK + beacon origins against it (issue #42). Closes the enrollment-instant MITM window.

**Architecture:** Compile-time constant in a gitignored per-deployment header (`master_pubkey_pin.h`). `Enrollment::processJoinAck` compares incoming `enrollment_public_key` against the 32-byte pin; `Mesh::processMasterBeacon` compares `origin_mac_address` against the 6-byte pin. Both gated by `!DEV_MODE`. Dual-master trust is transitive (secondary rides inside the authenticated primary JOIN_ACK).

**Tech Stack:** C++ (ESP-IDF / arduino-esp32), GoogleTest+Ctest, Python 3 for the small provisioning helper.

## Global Constraints

- No wire-format changes.
- Firmware-only + a tiny provisioning helper. Do not touch `lattice-protocol` or `lattice-hub`.
- Tiger-Style: pin checks are `memcmp` on stack, no allocations.
- `DEV_MODE` (`lattice::config::DEV_MODE`, `firmware/main/project_config.h:17`) bypasses BOTH checks. Compile-time constant; test bypass = `#ifdef UNIT_TEST` runtime flag (see Task 2).
- The correct proto field is `mesh_message.enrollment_public_key` (32 bytes) — verified against `Enrollment.cpp:68,119`. Do NOT use `msg.public_key` (typo in the design doc).
- Design doc: `docs/superpowers/specs/2026-08-04-phaseD-master-pubkey-pinning-design.md`.
- Parent umbrella: `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase D).

---

### Task 1: Provisioning scaffold — headers, tool, `.gitignore`, CMake

**Files:**
- Create: `firmware/main/config/master_pubkey_pin.h.example` — committed placeholder.
- Create: `firmware/main/config/master_pubkey_pin_wrapper.h` — committed dispatch header.
- Create: `tools/gen_master_pubkey_pin.py` — committed generator.
- Modify: `.gitignore` — add gitignored real header path.
- Modify: `tests/CMakeLists.txt` — add `LATTICE_ALLOW_EXAMPLE_PIN` compile flag for host-test build only; ensure `tests/mocks/` is included BEFORE `firmware/main/config/` in host builds so the test-mock header wins.
- Create: `tests/mocks/master_pubkey_pin.h` — test-fixed values.

**Interfaces:**
- Produces: `lattice::mesh::pin::MASTER_PUBKEY[32]` — 32-byte `constexpr uint8_t` array.
- Produces: `lattice::mesh::pin::MASTER_MAC[6]` — 6-byte `constexpr uint8_t` array.
- Produces: `master_pubkey_pin_wrapper.h` — the single header source-code sites include; it dispatches to real header, else `.example` under `LATTICE_ALLOW_EXAMPLE_PIN`, else `#error`.

- [ ] **Step 1: Write the placeholder pin header**

Create `firmware/main/config/master_pubkey_pin.h.example`:

```cpp
#pragma once
#include <cstdint>
namespace lattice { namespace mesh { namespace pin {
// PLACEHOLDER. Regenerate the real header via
//   python3 tools/gen_master_pubkey_pin.py <masterkey.json> <aa:bb:cc:dd:ee:ff>
constexpr uint8_t MASTER_PUBKEY[32] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t MASTER_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
}}}
```

- [ ] **Step 2: Write the wrapper header**

Create `firmware/main/config/master_pubkey_pin_wrapper.h`:

```cpp
#pragma once
// Deployer must generate firmware/main/config/master_pubkey_pin.h via
// tools/gen_master_pubkey_pin.py. Host-test builds and DEV_MODE firmware
// builds may set -DLATTICE_ALLOW_EXAMPLE_PIN=1 to fall back to the
// placeholder (compilable but not shippable).
#if __has_include("master_pubkey_pin.h")
  #include "master_pubkey_pin.h"
#elif defined(LATTICE_ALLOW_EXAMPLE_PIN)
  #include "master_pubkey_pin.h.example"
#else
  #error "firmware/main/config/master_pubkey_pin.h not found. Generate it via tools/gen_master_pubkey_pin.py or build with -DLATTICE_ALLOW_EXAMPLE_PIN=1 (DEV_MODE only)."
#endif
```

- [ ] **Step 3: Write the generator tool**

Create `tools/gen_master_pubkey_pin.py`:

```python
#!/usr/bin/env python3
"""Generate firmware/main/config/master_pubkey_pin.h from a masterkey.json
+ master MAC string. Idempotent — overwrites in place.

Usage: gen_master_pubkey_pin.py <masterkey.json> <aa:bb:cc:dd:ee:ff>
"""
import base64
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT = REPO_ROOT / "firmware/main/config/master_pubkey_pin.h"

def die(msg):
    print(f"gen_master_pubkey_pin: {msg}", file=sys.stderr)
    sys.exit(1)

def main(argv):
    if len(argv) != 3:
        die(f"usage: {argv[0]} <masterkey.json> <aa:bb:cc:dd:ee:ff>")
    keyfile = Path(argv[1])
    mac_str = argv[2]
    if not keyfile.is_file():
        die(f"masterkey.json not found: {keyfile}")
    data = json.loads(keyfile.read_text())
    pub_b64 = data.get("publicKey") or data.get("PublicKey")
    if not pub_b64:
        die("publicKey field not found in masterkey.json")
    pub = base64.b64decode(pub_b64)
    if len(pub) != 32:
        die(f"publicKey wrong length: {len(pub)}")
    mac_hex = re.sub(r"[^0-9a-fA-F]", "", mac_str)
    if len(mac_hex) != 12:
        die(f"MAC must be 6 bytes (12 hex chars): got {mac_str}")
    mac = bytes.fromhex(mac_hex)

    def as_c_array(b, per_line=8):
        rows = []
        for i in range(0, len(b), per_line):
            row = ", ".join(f"0x{byte:02X}" for byte in b[i:i+per_line])
            rows.append("    " + row + ",")
        return "\n".join(rows)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        "#pragma once\n"
        "#include <cstdint>\n"
        "// Generated by tools/gen_master_pubkey_pin.py — do not edit by hand.\n"
        "namespace lattice { namespace mesh { namespace pin {\n"
        f"constexpr uint8_t MASTER_PUBKEY[32] = {{\n{as_c_array(pub)}\n}};\n"
        f"constexpr uint8_t MASTER_MAC[6] = {{ {', '.join(f'0x{b:02X}' for b in mac)} }};\n"
        "}}}\n"
    )
    print(f"wrote {OUT}")

if __name__ == "__main__":
    main(sys.argv)
```

Make it executable: `chmod +x tools/gen_master_pubkey_pin.py`.

- [ ] **Step 4: Update `.gitignore`**

Append:

```
# per-deployment master pubkey pin (generated via tools/gen_master_pubkey_pin.py)
firmware/main/config/master_pubkey_pin.h
```

- [ ] **Step 5: Write the test-mocks pin header**

Create `tests/mocks/master_pubkey_pin.h`:

```cpp
#pragma once
#include <cstdint>
// Fixed test values used across all host-tests. Do NOT match production values.
namespace lattice { namespace mesh { namespace pin {
constexpr uint8_t MASTER_PUBKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
};
constexpr uint8_t MASTER_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 };
}}}
```

- [ ] **Step 6: Update `tests/CMakeLists.txt`**

Add `LATTICE_ALLOW_EXAMPLE_PIN` to the host-test target compile options AND ensure `tests/mocks/` is on the include path BEFORE `firmware/main/config/` so the mock wins over the placeholder.

```cmake
# In the block that sets up host-test targets:
target_compile_definitions(<test-target> PRIVATE LATTICE_ALLOW_EXAMPLE_PIN=1)
target_include_directories(<test-target> BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/mocks)
```

Verify the current `tests/CMakeLists.txt` structure before applying — the target names, include-dir order, and whether one central target or per-test targets exist. Use `grep -n "target_include_directories\|target_compile_definitions" tests/CMakeLists.txt` first.

- [ ] **Step 7: Verify build still compiles**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
```

Expected: builds pass (no source code uses the pin yet — Tasks 2/3 wire it in).

- [ ] **Step 8: Commit**

```bash
git add firmware/main/config/master_pubkey_pin.h.example \
        firmware/main/config/master_pubkey_pin_wrapper.h \
        tools/gen_master_pubkey_pin.py \
        .gitignore \
        tests/mocks/master_pubkey_pin.h \
        tests/CMakeLists.txt
git commit -m "feat(config): master pubkey pin scaffold — headers + generator + gitignore

Adds the provisioning surface for Phase D:
- master_pubkey_pin.h.example: committed placeholder (obviously fake values).
- master_pubkey_pin_wrapper.h: dispatch to real header, else .example under
  -DLATTICE_ALLOW_EXAMPLE_PIN, else #error.
- tools/gen_master_pubkey_pin.py: deployer generates the gitignored real
  header from hub's masterkey.json + master MAC.
- .gitignore: real header path.
- tests/mocks/master_pubkey_pin.h: fixed test values; host-test builds set
  LATTICE_ALLOW_EXAMPLE_PIN and include mocks/ BEFORE firmware/main/config/.

No source code uses the pin yet; Tasks 2/3 wire it into Enrollment and
processMasterBeacon.

Part of Phase D (issue #42)."
```

---

### Task 2: JOIN_ACK pubkey pin verification (`Enrollment`)

**Files:**
- Modify: `firmware/main/src/mesh/Enrollment.cpp` — insert pin check at the top of `processJoinAck` (~line 92).
- Test: `tests/unit/test_enrollment.cpp` — add 3 new cases.

**Interfaces:**
- Consumes: `lattice::mesh::pin::MASTER_PUBKEY` via `master_pubkey_pin_wrapper.h`.
- Consumes: `lattice::config::DEV_MODE` compile-time constant.
- Consumes (test-only): `lattice::mesh::pin::setTestBypass(bool)` — a `#ifdef UNIT_TEST`-gated runtime flag that lets tests toggle the pin check off (simulates DEV_MODE at runtime, since `DEV_MODE` is `constexpr`).
- Produces: pinbypass helper in the wrapper header (see Step 1).

- [ ] **Step 1: Add the test-bypass helper**

Extend `firmware/main/config/master_pubkey_pin_wrapper.h` after the include block:

```cpp
#ifdef UNIT_TEST
namespace lattice { namespace mesh { namespace pin {
// Test-only: when true, production check sites skip the pin comparison.
// Off by default so pin-active tests behave as production would.
inline bool& _testBypass() { static bool b = false; return b; }
inline void setTestBypass(bool on) { _testBypass() = on; }
inline bool isTestBypassed() { return _testBypass(); }
}}}
#else
namespace lattice { namespace mesh { namespace pin {
inline constexpr bool isTestBypassed() { return false; }
}}}
#endif
```

Firmware builds get the constexpr `false` — compiler folds the check to a bare `memcmp`. Host tests get the runtime toggle.

- [ ] **Step 2: Write the failing tests**

Add to `tests/unit/test_enrollment.cpp`. The existing fixture likely has an `EnrollmentTest` class; verify and match its shape.

```cpp
#include "config/master_pubkey_pin_wrapper.h"

class EnrollmentPinTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    // Ensure pin check is active for every test in this fixture unless
    // the test explicitly bypasses.
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }
};

TEST_F(EnrollmentPinTest, ProcessJoinAck_ValidPubkey_Enrolls) {
  // Build a JOIN_ACK whose enrollment_public_key matches the test pin.
  mesh_message ack{};
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);
  // Set origin_mac_address, target_mac_address, etc. per existing test fixture.
  // Feed to Enrollment::processJoinAck.
  Enrollment enrollment;
  enrollment.processJoinAck(ack, /*deviceMac*/nullptr, /*registerFn*/nullptr);
  EXPECT_TRUE(enrollment.hasMasterMac);
}

TEST_F(EnrollmentPinTest, ProcessJoinAck_WrongPubkey_DropsNoEnroll) {
  mesh_message ack{};
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);
  ack.enrollment_public_key[0] ^= 0xFF;   // corrupt first byte
  Enrollment enrollment;
  enrollment.processJoinAck(ack, nullptr, nullptr);
  EXPECT_FALSE(enrollment.hasMasterMac);
}

TEST_F(EnrollmentPinTest, ProcessJoinAck_TestBypass_SkipsCheck) {
  lattice::mesh::pin::setTestBypass(true);
  mesh_message ack{};
  memcpy(ack.enrollment_public_key, lattice::mesh::pin::MASTER_PUBKEY, 32);
  ack.enrollment_public_key[0] ^= 0xFF;   // wrong — but bypass active
  Enrollment enrollment;
  enrollment.processJoinAck(ack, nullptr, nullptr);
  EXPECT_TRUE(enrollment.hasMasterMac);
}
```

If any existing enrollment test builds a JOIN_ACK whose `enrollment_public_key` doesn't match the test pin, it now fails. Adapt those tests to seed the correct pin value (do NOT weaken assertions; keep whatever they were originally checking).

- [ ] **Step 3: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_enrollment --parallel
ctest --test-dir tests/build -R EnrollmentPinTest --output-on-failure
```

Expected: FAIL — pin check not yet in place, wrong-pubkey case still enrols.

- [ ] **Step 4: Insert the pin check in `processJoinAck`**

In `firmware/main/src/mesh/Enrollment.cpp`, near the top of `Enrollment::processJoinAck` (before the existing `hasMasterMac && memcmp(origin_mac_address, knownMasterMac...) != 0` check at ~line 105), add:

```cpp
#include "config/master_pubkey_pin_wrapper.h"

// ... inside processJoinAck, first thing after any deviceMac / self-echo drops ...
if (!lattice::config::DEV_MODE && !lattice::mesh::pin::isTestBypassed()) {
  if (memcmp(msg.enrollment_public_key,
             lattice::mesh::pin::MASTER_PUBKEY,
             sizeof(lattice::mesh::pin::MASTER_PUBKEY)) != 0) {
    Logger::logln("ENROLL", "JOIN_ACK master pubkey mismatch pin — drop",
                  LogLevel::LOG_ERROR);
    return;
  }
}
```

Verify the check runs BEFORE any state mutation or peer registration.

- [ ] **Step 5: Update any pre-existing enrollment test that fails**

Some existing tests build JOIN_ACKs with arbitrary pubkeys and expect enrollment to succeed. Two options per failing test:
  1. Update the fixture to seed `msg.enrollment_public_key` from `lattice::mesh::pin::MASTER_PUBKEY` — preferred if the test's purpose is unrelated to pin behaviour.
  2. Wrap the test with `pin::setTestBypass(true)` if the test explicitly needs to exercise pre-pin behaviour and re-fixturing would obscure intent.

Do NOT weaken any assertion. Verify each updated test still exercises its original invariant.

- [ ] **Step 6: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_enrollment --parallel
ctest --test-dir tests/build -R "EnrollmentPinTest|EnrollmentTest" --output-on-failure
```

Expected: 3 new PASS + all existing (possibly adapted) PASS.

- [ ] **Step 7: Full unit suite for regressions**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 8: Commit**

```bash
git add firmware/main/config/master_pubkey_pin_wrapper.h \
        firmware/main/src/mesh/Enrollment.cpp \
        tests/unit/test_enrollment.cpp
git commit -m "feat(enroll): verify JOIN_ACK master pubkey against compile-time pin

Enrollment::processJoinAck now rejects any JOIN_ACK whose
enrollment_public_key does not match lattice::mesh::pin::MASTER_PUBKEY
(the deployment-provisioned 32-byte master pubkey). DEV_MODE bypasses
the check; a UNIT_TEST-only runtime bypass lets tests toggle without
recompilation.

Strong authentication: an RF-present attacker cannot forge JOIN_ACK
without the master's private key. Closes the enrollment-instant MITM
window on the pubkey side.

Part of Phase D (issue #42)."
```

---

### Task 3: Beacon origin MAC pin verification (`Mesh::processMasterBeacon`) + boot warn

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.cpp` — insert MAC pin check at the top of `processMasterBeacon` (~line 712); add DEV_MODE WARN log in `Mesh::init`.
- Test: `tests/unit/test_mesh_logic.cpp` — add 3 new cases.

**Interfaces:**
- Consumes: `lattice::mesh::pin::MASTER_MAC` via wrapper.
- Consumes: `lattice::config::DEV_MODE`, `lattice::mesh::pin::isTestBypassed()`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_mesh_logic.cpp`:

```cpp
#include "config/master_pubkey_pin_wrapper.h"

class MeshBeaconPinTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    lattice::mesh::pin::setTestBypass(false);
  }
  void TearDown() override {
    lattice::mesh::pin::setTestBypass(false);
  }
};

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_ValidOriginMac_Learns) {
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  memcpy(b.last_hop_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  b.hop_count = 0;
  m.processMasterBeacon(b);
  EXPECT_TRUE(m.enrollment.hasMasterMac);   // access via existing UNIT_TEST public: pattern
}

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_WrongOriginMac_Drops) {
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.origin_mac_address[0] ^= 0xFF;
  memcpy(b.last_hop_mac_address, b.origin_mac_address, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  m.processMasterBeacon(b);
  EXPECT_FALSE(m.enrollment.hasMasterMac);
}

TEST_F(MeshBeaconPinTest, ProcessMasterBeacon_TestBypass_SkipsCheck) {
  lattice::mesh::pin::setTestBypass(true);
  lattice::mesh::Mesh m;
  mesh_message b{};
  memcpy(b.origin_mac_address, lattice::mesh::pin::MASTER_MAC, 6);
  b.origin_mac_address[0] ^= 0xFF;
  memcpy(b.last_hop_mac_address, b.origin_mac_address, 6);
  b.message_type = MESH_TYPE_MASTER_BEACON;
  m.processMasterBeacon(b);
  EXPECT_TRUE(m.enrollment.hasMasterMac);
}
```

Any existing beacon test that uses an origin MAC differing from `pin::MASTER_MAC` will fail. Update per the same rule as Task 2 Step 5.

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R MeshBeaconPinTest --output-on-failure
```

Expected: FAIL — wrong-MAC beacon still learns.

- [ ] **Step 3: Insert the MAC pin check in `processMasterBeacon`**

At the top of `Mesh::processMasterBeacon` (`firmware/main/src/mesh/Mesh.cpp:712`), right after the self-echo guard:

```cpp
if (!lattice::config::DEV_MODE && !lattice::mesh::pin::isTestBypassed()) {
  if (memcmp(msg.origin_mac_address,
             lattice::mesh::pin::MASTER_MAC,
             sizeof(lattice::mesh::pin::MASTER_MAC)) != 0) {
    Logger::logln("MESH", "Beacon origin MAC mismatch pin — drop",
                  LogLevel::LOG_ERROR);
    return;
  }
}
```

Placement: after self-echo drop, before hop-count guard. Ensures the pin filter runs before any state effects.

Add `#include "config/master_pubkey_pin_wrapper.h"` at the top of `Mesh.cpp` if not already included.

- [ ] **Step 4: Add boot-time DEV_MODE WARN in `Mesh::init`**

Near the top of `Mesh::init` (`firmware/main/src/mesh/Mesh.cpp:229`), just after the "Boot epoch" log line, add:

```cpp
if (lattice::config::DEV_MODE) {
  Logger::logln("MESH", "DEV_MODE: master pubkey pin disabled — do not ship this build",
                LogLevel::LOG_WARN);
}
```

- [ ] **Step 5: Update pre-existing beacon tests as needed**

Same rule as Task 2 Step 5. Prefer fixture seeding over runtime bypass unless the test's purpose is exercising pre-pin behaviour.

- [ ] **Step 6: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R "MeshBeaconPinTest|MeshTest|MeshLogicTest" --output-on-failure
```

- [ ] **Step 7: Full unit suite for regressions**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 8: Commit**

```bash
git add firmware/main/src/mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "feat(mesh): verify beacon origin MAC against compile-time pin

processMasterBeacon drops beacons whose origin_mac_address does not
match lattice::mesh::pin::MASTER_MAC. DEV_MODE bypasses.

Weaker guarantee than the JOIN_ACK pubkey pin — WiFi MACs are
trivially spoofable, so a MAC-spoofing RF attacker bypasses this
check. Rejects naive attackers; real beacon auth would need a signed
field in the beacon (wire change, v5). Documented as follow-up.

Also adds a boot-time WARN when DEV_MODE is compiled in, so the
pin-disabled state is visible in operator logs.

Part of Phase D (issue #42)."
```

---

### Task 4: E2E fixture seed + PR

**Files:**
- Modify (if needed): `tests/e2e/scenarios/test_enrollment_e2e.cpp` — seed sim master's pubkey/MAC into the test pin, OR set `pin::setTestBypass(true)` in the e2e SetUp per pragmatic decision at implementation time.
- Modify: `.superpowers/sdd/phaseD-master-pubkey-pinning/progress.md` — new SDD ledger (gitignored).

**Interfaces:**
- Consumes: e2e SimNode + FakeHub fixtures.

- [ ] **Step 1: Full unit suite green**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 2: E2E enrollment scenarios green**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 -L e2e -R "enroll|beacon"
```

If any e2e scenario fails because the sim's JOIN_ACK carries a pubkey/MAC that doesn't match the test pin, decide per case:
- Update the sim's master pubkey / MAC generator to use the pin's values (preferred — keeps e2e in the pin-active regime).
- Or wrap the scenario with `pin::setTestBypass(true)` in a SetUp helper (falls back to pre-pin behaviour — use only if seeding the pin into the sim is infeasible).

- [ ] **Step 3: Write SDD ledger**

Create `.superpowers/sdd/phaseD-master-pubkey-pinning/progress.md`:

```markdown
# SDD ledger — plan: docs/superpowers/plans/2026-08-04-phaseD-master-pubkey-pinning.md
Task 1: complete (commit <sha>) — provisioning scaffold
Task 2: complete (commit <sha>) — JOIN_ACK pubkey pin
Task 3: complete (commit <sha>) — beacon MAC pin + DEV_MODE warn
Task 4: complete — full suite green
```

- [ ] **Step 4: Push branch + open PR**

```bash
git push -u origin feat/phaseD-master-pubkey-pinning
gh pr create --title "feat(phaseD): master-pubkey pinning (closes #42)" \
             --body "$(cat <<'EOF'
Implements docs/superpowers/plans/2026-08-04-phaseD-master-pubkey-pinning.md.

Closes #42.

## Summary
- Compile-time master pubkey + MAC pin via gitignored per-deployment header (tools/gen_master_pubkey_pin.py generates from hub's masterkey.json).
- Enrollment::processJoinAck rejects JOIN_ACK whose enrollment_public_key ≠ pin (strong auth).
- Mesh::processMasterBeacon drops beacons whose origin_mac_address ≠ pin (weaker; MACs spoofable — documented).
- DEV_MODE bypasses both checks + prints boot-time WARN.
- Dual-master trust transitive: primary's authenticated JOIN_ACK carries the secondary pubkey.

## Test plan
- [x] Host unit suite green.
- [x] E2E enrollment scenarios green.
- [ ] CI green.
EOF
)"
```

---

## Self-review

**Spec coverage:**
- §Design/1 (provisioning) → Task 1.
- §Design/2 (JOIN_ACK pubkey check) → Task 2.
- §Design/3 (beacon MAC check) → Task 3.
- §Design/4 (DEV_MODE bypass + boot WARN) → Task 2 (JOIN_ACK gate) + Task 3 (WARN log).
- §Design/5 (dual-master transitive) → no code needed; documented in commit + PR body.
- §Design/6 (error handling) → drop+log semantics implemented per Tasks 2/3.
- §Design/7 (testing) → 3+3 tests split across Tasks 2/3; e2e fixture seed in Task 4.
- §Design/8 (non-goals) → respected.

**Type consistency:**
- `lattice::mesh::pin::MASTER_PUBKEY[32]` — declared Task 1 Steps 1/5, consumed Task 2 Steps 2/4.
- `lattice::mesh::pin::MASTER_MAC[6]` — declared same, consumed Task 3 Steps 1/3.
- `lattice::mesh::pin::isTestBypassed()` — declared Task 2 Step 1 (constexpr false in production, runtime var under UNIT_TEST), consumed Task 2 Step 4 + Task 3 Step 3.
- `lattice::mesh::pin::setTestBypass(bool)` — declared Task 2 Step 1, consumed by every test fixture in Tasks 2/3.
- `msg.enrollment_public_key` (32 bytes) — correct proto field name per `Enrollment.cpp:68,119`.
- `msg.origin_mac_address` (6 bytes) — matches processMasterBeacon signature.

**Placeholder scan:**
- Task 1 Step 6 says "verify the current tests/CMakeLists.txt structure before applying". This is a legitimate implementer-time verify with a concrete grep to resolve — not a hidden TODO.
- Task 2 Step 2 test fixture references "the existing fixture likely has an EnrollmentTest class; verify and match its shape" — same class of implementer-time verify. Test-body code is provided verbatim; only the fixture-inheritance detail needs local confirmation.
- Task 4 PR body has a HEREDOC template — implementer expands the empty CI checkbox after landing.

**Scope check:** 4 tasks, one repo, one design doc, single PR — appropriate for one plan.
