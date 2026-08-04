# Phase E — Hygiene sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close 6 low-severity hygiene items grouped as issue #47 per Phase E of the umbrella spec.

**Architecture:** Five nodes-side items (RAII guard, signed-shift cast, serial `proto_version`, downlink clamp, LRU runtime guard) land as one nodes PR. One protocol-side item (`gofmt`) lands as a separate small protocol PR.

**Tech Stack:** C++ (ESP-IDF), GoogleTest+Ctest, Go 1.21 (protocol gofmt).

## Global Constraints

- No wire-format changes.
- No functional behaviour change in shipped configuration.
- Firmware-side: RAII guard is header-only; no dynamic allocation on the hot path.
- Design doc: `docs/superpowers/specs/2026-08-04-phaseE-hygiene-design.md`.
- Parent umbrella (Phase E): `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md`.
- Under `UNIT_TEST`, `err::fail`/`err::fatal` throws (Phase A/B/C/D convention) — the RAII guard fixes the mbedtls context leak that Phase E items #1 targets.
- `PROTO_VERSION` is currently `4` post-Phase C flag-day (`firmware/main/src/mesh/Mesh.h:39`).

---

### Task 1: mbedtls RAII guard + swap in E2ECrypto.h + MeshCrypto.h

**Files:**
- Create: `firmware/main/src/mesh/MbedtlsGuard.h` (header-only guard types).
- Modify: `firmware/main/src/mesh/E2ECrypto.h` — `computeSharedSecret`, `deriveE2EKeys` swap raw contexts for guards; drop redundant trailing `_free` calls.
- Modify: `firmware/main/src/mesh/MeshCrypto.h` — `generateKeypair` same swap.
- Test: existing `tests/unit/test_e2e_crypto.cpp` regression — no new cases required; verify pass.

**Interfaces:**
- Produces: `lattice::mesh::mbedtls_guard::EcdhCtx`, `EntropyCtx`, `CtrDrbgCtx`, `MdCtx` (add whichever contexts the current code uses; verify by grep).
- Each guard type: default-init calls the matching `mbedtls_*_init`; destructor calls the matching `mbedtls_*_free`; implicit conversion `operator T*()` to the raw pointer so existing mbedtls call sites compile with a one-token change.

- [ ] **Step 1: Write the guard header**

Create `firmware/main/src/mesh/MbedtlsGuard.h`:

```cpp
#pragma once
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>

namespace lattice { namespace mesh { namespace mbedtls_guard {

struct EcdhCtx {
  mbedtls_ecdh_context ctx;
  EcdhCtx()  { mbedtls_ecdh_init(&ctx); }
  ~EcdhCtx() { mbedtls_ecdh_free(&ctx); }
  EcdhCtx(const EcdhCtx&) = delete;
  EcdhCtx& operator=(const EcdhCtx&) = delete;
  operator mbedtls_ecdh_context*() { return &ctx; }
};

struct EntropyCtx {
  mbedtls_entropy_context ctx;
  EntropyCtx()  { mbedtls_entropy_init(&ctx); }
  ~EntropyCtx() { mbedtls_entropy_free(&ctx); }
  EntropyCtx(const EntropyCtx&) = delete;
  EntropyCtx& operator=(const EntropyCtx&) = delete;
  operator mbedtls_entropy_context*() { return &ctx; }
};

struct CtrDrbgCtx {
  mbedtls_ctr_drbg_context ctx;
  CtrDrbgCtx()  { mbedtls_ctr_drbg_init(&ctx); }
  ~CtrDrbgCtx() { mbedtls_ctr_drbg_free(&ctx); }
  CtrDrbgCtx(const CtrDrbgCtx&) = delete;
  CtrDrbgCtx& operator=(const CtrDrbgCtx&) = delete;
  operator mbedtls_ctr_drbg_context*() { return &ctx; }
};

struct MdCtx {
  mbedtls_md_context_t ctx;
  MdCtx()  { mbedtls_md_init(&ctx); }
  ~MdCtx() { mbedtls_md_free(&ctx); }
  MdCtx(const MdCtx&) = delete;
  MdCtx& operator=(const MdCtx&) = delete;
  operator mbedtls_md_context_t*() { return &ctx; }
};

}}}  // namespace lattice::mesh::mbedtls_guard
```

Only include `MdCtx` if `mbedtls_md_*` is used in the target files — verify via `grep -n "mbedtls_md" firmware/main/src/mesh/E2ECrypto.h firmware/main/src/mesh/MeshCrypto.h firmware/main/src/mesh/RouteMac.h`. If not used at that scope, drop `MdCtx` from the header (RouteMac.h uses `mbedtls_md_hmac` which is a one-shot free-standing call and does NOT need a guard).

- [ ] **Step 2: Swap contexts in `E2ECrypto.h`**

For each function (`computeSharedSecret`, `deriveE2EKeys`):

- Replace `mbedtls_ecdh_context ecdh;` + `mbedtls_ecdh_init(&ecdh);` with `lattice::mesh::mbedtls_guard::EcdhCtx ecdh;` (guard's default-ctor inits).
- Same for `mbedtls_entropy_context` → `EntropyCtx`, `mbedtls_ctr_drbg_context` → `CtrDrbgCtx`.
- Every mbedtls API call site keeps `&ecdh` etc. or the raw `ecdh` — the guard's `operator T*()` provides the pointer. Verify by grep that no site uses `.ctx` explicitly.
- Delete trailing `mbedtls_*_free(&…)` calls on success paths (guard's dtor handles them).
- Fatal paths (`err::fail(...)`) can just `return` after the call — guard dtor unwinds under `UNIT_TEST`, and production is `[[noreturn]]` (reboot), leak moot.

Add `#include "MbedtlsGuard.h"` at the top of E2ECrypto.h.

- [ ] **Step 3: Swap contexts in `MeshCrypto.h`**

Same treatment for `generateKeypair` (and any other function using the mbedtls context pattern). `#include "MbedtlsGuard.h"` at the top.

- [ ] **Step 4: Build + full E2E crypto unit suite regression**

```bash
cmake --build tests/build --target test_e2e_crypto --parallel
ctest --test-dir tests/build -R "e2e_crypto|E2ECrypto|Enrollment|MeshCrypto" --output-on-failure
```

Expected: all PASS. Guard swap is behaviour-preserving.

- [ ] **Step 5: Full unit suite regression**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/mesh/MbedtlsGuard.h \
        firmware/main/src/mesh/E2ECrypto.h \
        firmware/main/src/mesh/MeshCrypto.h
git commit -m "refactor(mesh): RAII guards for mbedtls contexts

Under UNIT_TEST, err::fail unwinds instead of rebooting, leaking every
mbedtls_ecdh_context / entropy / ctr_drbg initialised on the failing
function's stack. Production is untouched ([[noreturn]] reboot).

Adds MbedtlsGuard.h with header-only RAII wrappers (EcdhCtx,
EntropyCtx, CtrDrbgCtx). Swaps raw contexts across E2ECrypto.h
(computeSharedSecret, deriveE2EKeys) and MeshCrypto.h (generateKeypair)
for the guards. Success paths drop the redundant trailing _free calls;
fatal paths auto-clean via dtors.

Part of Phase E (issue #47 item 1)."
```

---

### Task 2: Signed-shift cast + serial `proto_version` literal

**Files:**
- Modify: `firmware/main/src/mesh/E2ECrypto.h` — cast `msg.data_type` to `uint32_t` in `buildAad` (and `buildNonce` if same pattern).
- Modify: `firmware/main/src/adapter/serial/SerialAdapter.cpp` — `msg.proto_version = 1;` → `= lattice::mesh::PROTO_VERSION;` at ~line 158.
- Test: existing `tests/unit/test_e2e_crypto.cpp` regression; verify no behaviour change on non-negative `data_type`.

**Interfaces:**
- Consumes: `lattice::mesh::PROTO_VERSION` (currently `4`, defined in `firmware/main/src/mesh/Mesh.h:39`).

- [ ] **Step 1: Fix signed-shift in `buildAad`**

In `firmware/main/src/mesh/E2ECrypto.h::buildAad` (~lines 120-122):

```cpp
uint32_t dt = static_cast<uint32_t>(msg.data_type);
aad[N+0] = static_cast<uint8_t>(dt);
aad[N+1] = static_cast<uint8_t>(dt >> 8);
aad[N+2] = static_cast<uint8_t>(dt >> 16);
aad[N+3] = static_cast<uint8_t>(dt >> 24);
```

Verify `buildNonce` — grep for `data_type >>` — if the same pattern exists, apply the same cast.

- [ ] **Step 2: Fix serial proto_version literal**

In `firmware/main/src/adapter/serial/SerialAdapter.cpp` at ~line 158 (verify with `grep -n "proto_version = 1" firmware/main/src/adapter/serial/SerialAdapter.cpp`):

```cpp
msg.proto_version = lattice::mesh::PROTO_VERSION;
```

Add `#include "src/mesh/Mesh.h"` at the top of `SerialAdapter.cpp` if not already indirectly present. Actually — `Mesh.h` may not be intended for adapter code. Two options:
- (a) Include `Mesh.h` directly.
- (b) Extract `PROTO_VERSION` into a shared constants header (heavier).

Prefer (a) unless it introduces a circular dependency — grep first: `grep -n "#include.*Mesh.h" firmware/main/src/adapter/serial/`. If Mesh.h already indirectly included via some other header, no change needed. If a circular is real, define a local `SERIAL_PROTO_VERSION` inline and add a `static_assert(SERIAL_PROTO_VERSION == lattice::mesh::PROTO_VERSION, ...)` to catch future drift.

- [ ] **Step 3: Run tests**

```bash
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/src/mesh/E2ECrypto.h \
        firmware/main/src/adapter/serial/SerialAdapter.cpp
git commit -m "fix(mesh): portable signed shift + serial proto_version tracks PROTO_VERSION

E2ECrypto.h::buildAad casts msg.data_type to uint32_t before shifting.
Right-shift of a negative signed value is implementation-defined in C++;
benign today (data_type holds small non-negative adapter enums), fixed
for portability.

SerialAdapter.cpp::relayEnrollmentToServer set msg.proto_version = 1 —
inconsistent with the flag-day PROTO_VERSION = 4 (post-Phase C). Point
at the shared constant instead.

Part of Phase E (issue #47 items 2 + 3)."
```

---

### Task 3: `sendDownlinkToNode` clamp + LRU runtime guard + tests

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.cpp` — inline `route_len <= MAX_HOPS` clamp in `sendDownlinkToNode`; runtime enrolled/master re-check in `registerDownlinkPeer` LRU-touch branch.
- Test: `tests/unit/test_mesh_logic.cpp` — new fixture cases `MeshDownlinkClampTest` (1 case) and `RegisterDownlinkPeerTest.LRUEntryBecomesEnrolled_EvictsOnTouch`.

**Interfaces:**
- Consumes: `lattice::config::MAX_HOPS`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_mesh_logic.cpp` (fixture pattern per prior Phase D — direct member access via `#ifdef UNIT_TEST public:`):

```cpp
TEST(MeshDownlinkClamp, OversizedRouteLen_Drops) {
  lattice::mesh::Mesh m;
  m.setIsMaster(true);
  m.init();
  ASSERT_NE(m.testRoutes(), nullptr);
  // Directly poke a bogus oversized entry into RouteTable, then call the
  // downlink send path. Assert no crash and no frame transmitted.
  //
  // If direct poke is infeasible (RouteTable API guards size), skip this
  // test — the clamp is defense-in-depth. Report the skip in the task
  // report rather than weakening the assertion.
  //
  // Alternatively, extract a static helper for the clamp check and unit
  // test the helper.
}

TEST(RegisterDownlinkPeer, LRUEntryBecomesEnrolled_EvictsOnTouch) {
  lattice::mesh::Mesh m;
  uint8_t mac[6] = {0x01,0x02,0x03,0x04,0x05,0x06};
  // 1. Register mac into the LRU (call registerDownlinkPeer once for a
  //    MAC not in peers).
  m.registerDownlinkPeer(mac);
  EXPECT_EQ(m.downlinkPeerLruCount, 1);
  // 2. Add mac to peers (enrolled).
  //    Use whichever PeerRegistry test-hook exists in this repo; grep first.
  //    e.g. m.peers.addForTest(mac, dummyPubKey).
  // 3. Call registerDownlinkPeer(mac) again.
  m.registerDownlinkPeer(mac);
  // 4. Assert LRU no longer contains mac.
  EXPECT_EQ(m.downlinkPeerLruCount, 0);
}
```

Fixture and helper names verified at implementation time — the "direct member access via UNIT_TEST public:" pattern is confirmed working from Phase A/B/D tests. `PeerRegistry::addForTest` may or may not exist — verify with `grep -n "addForTest\|friend class" firmware/main/src/mesh/PeerRegistry.h`; if not present, add a `friend class` or a small test hook.

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R "MeshDownlinkClamp|RegisterDownlinkPeer" --output-on-failure
```

Expected: FAIL — clamp and LRU-guard not yet present.

- [ ] **Step 3: Add the `sendDownlinkToNode` clamp**

Find the site in `firmware/main/src/mesh/Mesh.cpp`:

```bash
grep -n "sendDownlinkToNode\|routes->lookup" firmware/main/src/mesh/Mesh.cpp
```

In the branch that assigns `msg.route_len = pathLen` (or similar), before the assignment:

```cpp
if (pathLen > lattice::config::MAX_HOPS) {
  Logger::logln("MESH", "downlink route_len exceeds MAX_HOPS — dropping",
                LogLevel::LOG_ERROR);
  return;
}
```

- [ ] **Step 4: Add the LRU runtime guard**

In `firmware/main/src/mesh/Mesh.cpp::registerDownlinkPeer`, inside the LRU-touch `for` loop (the branch that finds an existing entry and moves it to front), before the existing "touch" logic:

```cpp
// Defense-in-depth (issue #47 item 5): if this MAC has since become
// enrolled or the current master, its peering is now owned by that path.
// Evict from LRU.
if (peers.find(mac) ||
    (currentMaster.distance != 0xFF &&
     lattice::utils::MacAddress(mac) == lattice::utils::MacAddress(currentMaster.mac))) {
  for (size_t j = i; j + 1 < downlinkPeerLruCount; ++j) {
    memcpy(downlinkPeerLru[j], downlinkPeerLru[j + 1], 6);
  }
  downlinkPeerLruCount--;
  lattice::mesh::crypto::registerPeerWithEspNow(mac);
  return;
}
// ... existing touch logic ...
```

- [ ] **Step 5: Run tests to verify all pass**

```bash
cmake --build tests/build --target test_mesh_logic --parallel
ctest --test-dir tests/build -R "MeshDownlinkClamp|RegisterDownlinkPeer|MeshTest|MeshLogicTest" --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Full unit suite for regressions**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 7: Commit**

```bash
git add firmware/main/src/mesh/Mesh.cpp tests/unit/test_mesh_logic.cpp
git commit -m "fix(mesh): defensive clamp in sendDownlinkToNode + LRU runtime guard

Two defense-in-depth items from Phase E:

- sendDownlinkToNode now drops any downlink whose route_len exceeds
  MAX_HOPS before indexing route_path. RouteTable's write-time guard
  already bounds this transitively; the inline check makes the bound
  local and refactor-proof.

- registerDownlinkPeer's LRU-touch path now re-checks whether the MAC
  has since become enrolled or the current master; if so, evicts from
  the LRU (its peering is owned by peers / currentMaster). Guards the
  theoretical case where a MAC's status changes after LRU insertion.

Part of Phase E (issue #47 items 4 + 5)."
```

---

### Task 4: E2E verify + nodes PR

**Files:**
- No production changes.
- Modify: `.superpowers/sdd/phaseE-hygiene/progress.md` (gitignored).

- [ ] **Step 1: Full unit suite green**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

- [ ] **Step 2: E2E crypto + enrollment scenarios green**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 -L e2e -R "aead|enroll|route"
```

- [ ] **Step 3: SDD ledger**

Append Task 4 completion line to the workspace `progress.md`.

- [ ] **Step 4: Push + open PR**

```bash
git push -u origin feat/phaseE-hygiene
gh pr create --title "feat(phaseE): hygiene sweep (closes #47 nodes side)" \
             --body "$(cat <<'EOF'
Implements 5 of 6 items from docs/superpowers/plans/2026-08-04-phaseE-hygiene.md
(item 6 gofmt is a separate protocol-side PR).

Closes #47 on the nodes side.

## Summary
- Item 1: RAII guards for mbedtls contexts (E2ECrypto.h, MeshCrypto.h) — closes UNIT_TEST leak.
- Item 2: cast msg.data_type to uint32_t before shifting (buildAad).
- Item 3: SerialAdapter.cpp proto_version literal → PROTO_VERSION constant.
- Item 4: inline route_len <= MAX_HOPS clamp in sendDownlinkToNode.
- Item 5: LRU runtime enrolled/master re-check in registerDownlinkPeer.

## Test plan
- [x] Host unit suite green.
- [x] E2E aead + enroll + route scenarios green.
- [ ] CI green.
EOF
)"
```

---

### Task 5: Protocol `gofmt` sweep

**Repo:** `/Users/benji/projects/personal/lattice-protocol` — branch `chore/phaseE-gofmt`.

**Files:**
- Modify: `message/message.go` — whitespace only.

- [ ] **Step 1: Sync + branch**

```bash
cd /Users/benji/projects/personal/lattice-protocol
git checkout main
git pull --ff-only
git checkout -b chore/phaseE-gofmt
```

- [ ] **Step 2: Run gofmt**

```bash
gofmt -w message/message.go
```

- [ ] **Step 3: Verify no drift**

```bash
gofmt -l .
make check
go test ./...
```

Expected: `gofmt -l .` prints nothing (all files formatted). `make check` clean (regen produces byte-identical `c/*.h` and `proto/mesh.proto`).

- [ ] **Step 4: Commit + push + PR**

```bash
git add message/message.go
git commit -m "style: gofmt message/message.go

Whitespace fix flagged by gofmt -l (stray double-space before type
names on proto-v3 field lines). No wire change; make check clean.

Part of Phase E (lattice-nodes issue #47 item 6)."
git push -u origin chore/phaseE-gofmt
gh pr create --title "style: gofmt message/message.go" \
             --body "Whitespace fix. No wire change. Part of Phase E hygiene (lattice-nodes#47 item 6)."
```

---

## Self-review

**Spec coverage:**
- §Design/1 (mbedtls RAII) → Task 1.
- §Design/2 (signed shift) → Task 2 Step 1.
- §Design/3 (serial proto_version) → Task 2 Step 2.
- §Design/4 (downlink clamp) → Task 3 Steps 1/3.
- §Design/5 (LRU guard) → Task 3 Steps 1/4.
- §Design/6 (gofmt) → Task 5.

**Type consistency:**
- `lattice::mesh::mbedtls_guard::{EcdhCtx, EntropyCtx, CtrDrbgCtx}` — declared Task 1 Step 1, consumed Task 1 Steps 2-3.
- `lattice::mesh::PROTO_VERSION` — pre-existing, consumed Task 2 Step 2.
- `lattice::config::MAX_HOPS` — pre-existing, consumed Task 3 Step 3.

**Placeholder scan:** none. Task 3 Step 1's test skeleton has legitimate "verify at implementation time" hooks (`PeerRegistry::addForTest` may need adding) with concrete grep commands to resolve.

**Scope check:** 5 tasks — 4 nodes (bundled into 1 PR), 1 protocol (separate PR). Appropriate for one plan.
