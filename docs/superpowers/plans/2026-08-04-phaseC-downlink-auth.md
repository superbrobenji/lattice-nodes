# Phase C — Downlink auth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Authenticate the relay-accumulated `route_path` via a chained HMAC-SHA256-64 keyed off pairwise `k_up` (issue #44). Cross-repo: protocol wire bump + nodes MAC-and-verify + hub dep bump.

**Architecture:** New `AuthPath[8]` wire field in `MeshMessage` (protocol v0.5.0, flag-day). Origin node seeds `mac_0`; each relay chains its hop's HMAC into the field. Master reconstructs from `k_up` for each hop in `route_path`, compares to wire value; drops+logs on mismatch, records into `RouteTable` only on pass. Hub does not participate — bumps proto dep + `ProtoVersion` gate 3→4.

**Tech Stack:** Go 1.21 + reflection codegen (protocol), C++ (ESP-IDF/arduino-esp32) + mbedtls HMAC-SHA256 (nodes), Go 1.23 + protoc-gen-go v1.36.11 (hub).

## Global Constraints

- Flag-day; no backwards compatibility with protocol v0.4.x. `ProtoVersion` bumps from 3 to 4 atomically at hub merge.
- `WireSize` == 250 bytes exactly (ESP-NOW payload cap). `static_assert` in generated `c/mesh_message.h`.
- No new persisted state on any node.
- Nodes: Tiger-Style — HMAC computation is stack-only, fixed-size buffers.
- Under `UNIT_TEST`, `err::fail` throws `FatalError` (unchanged from Phase A/B).
- Design doc (canonical): `lattice-nodes/docs/superpowers/specs/2026-08-04-phaseC-downlink-auth-design.md`.
- Parent umbrella (Phase C): `lattice-nodes/docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md`.
- Umbrella "release-flow rule": protocol PR merges + `v0.5.0` tag is pushed BEFORE nodes/hub PRs open. Nodes + hub can be prepared in parallel after the tag.
- Every hop in the chain uses the same secret: the reporting node's pairwise `k_up` with the master. `prev_hop` is zeroed for the originating hop.

---

### Task 1: Protocol — add `AuthPath[8]` field + regen (lattice-protocol)

**Repo:** `/Users/benji/projects/personal/lattice-protocol` — branch `feat/phaseC-authpath`.

**Files:**
- Modify: `message/message.go` — add `AuthPath [8]byte` field.
- Modify: `README.md` — versioning table row for v0.5.0.
- Regen: `c/mesh_message.h`, `proto/mesh.proto` (via `go generate ./...`).

**Interfaces:**
- Produces: `MeshMessage.AuthPath` wire field (proto tag 17, C name `auth_path`, 8 bytes fixed).
- Produces: `WireSize == 250` (currently 242).

- [ ] **Step 1: Add the field**

In `message/message.go`, inside the `MeshMessage` struct, after `SecondaryPublicKey`:

```go
AuthPath [8]byte `c:"uint8_t[8]" proto:"17,bytes,opt,authPath"`
```

Update the `WireSize` constant:

```go
const WireSize = 250
```

- [ ] **Step 2: Regenerate + verify `make check` passes**

```bash
cd /Users/benji/projects/personal/lattice-protocol
go generate ./...
make check
```

Expected: `git diff --exit-code c/ proto/` reports zero drift after regen; `c/mesh_message.h` shows the new `uint8_t auth_path[8];` field and updated `static_assert(sizeof(mesh_message) == 250, ...)`.

- [ ] **Step 3: Run Go tests**

```bash
go test ./...
```

Expected: all packages pass (protocol Go layer has no tests that depend on wire size directly beyond codegen validation).

- [ ] **Step 4: Update README versioning table**

Prepend to `README.md`'s `| Tag | Notes |` table:

```
| v0.5.0 | Protocol v4 wire: AuthPath[8] — chained HMAC-SHA256-64 authenticating relay-accumulated route_path (issue lattice-nodes#44). Flag-day, no backcompat. WireSize=250. |
```

- [ ] **Step 5: Commit + push + open PR**

```bash
git add message/message.go c/mesh_message.h proto/mesh.proto README.md
git commit -m "feat: protocol v4 — AuthPath[8] chained MAC over route_path

Adds AuthPath[8] to MeshMessage: chained HMAC-SHA256 truncated to 64 bits
over the relay-accumulated route_path, keyed off the reporting hop's
pairwise k_up with the master. Firmware-side field — server tolerates
(hub-side verification not in this design; master owns path auth).

WireSize 242 → 250 (exactly at ESP-NOW payload cap). Flag-day: bumps
protocol to v4; no backcompat.

Closes lattice-nodes#44 (protocol side)."
git push -u origin feat/phaseC-authpath
gh pr create --title "feat: protocol v4 — AuthPath[8] chained MAC over route_path" \
             --body "Phase C wire change per lattice-nodes design doc. After merge, tag v0.5.0."
```

- [ ] **Step 6: HANDOFF — merge PR, then tag v0.5.0**

```bash
# after PR is approved and merged:
git fetch --all --prune
git checkout main
git merge --ff-only origin/main
git tag -a v0.5.0 -m "v0.5.0: protocol v4 — AuthPath[8] chained MAC (route_path authentication)"
git push origin v0.5.0
```

Task 1 is not "complete" until v0.5.0 is pushed. Tasks 2 and 3 both depend on the tag being available.

---

### Task 2: Nodes — chain MAC + master verification (lattice-nodes)

**Repo:** `/Users/benji/projects/personal/lattice-nodes` — branch `feat/phaseC-downlink-auth`.

**Precondition:** protocol `v0.5.0` tag exists on origin.

**Files:**
- Update: `firmware/main/lib/lattice-protocol` submodule pin → `v0.5.0`.
- Create: `firmware/main/src/mesh/RouteMac.h` (header-only, mirrors `E2ECrypto.h` pattern).
- Modify: `firmware/main/src/mesh/Mesh.cpp` — accumulate `auth_path` in the OpRouteReport originate + relay paths; verify + drop in `processRouteReport` before `routes->record`.
- Modify: `firmware/main/CMakeLists.txt` if `RouteMac.h` needs adding (header-only, likely no change since headers are picked up via include dirs; verify).
- Test: `tests/unit/test_route_mac.cpp` (new).
- Test: `tests/unit/test_route_report.cpp` — extend with 4 new cases.
- Test: `tests/e2e/scenarios/test_route_report_e2e.cpp` — extend with 2 new scenarios.

**Interfaces:**
- Consumes: `mbedtls_md_hmac(MBEDTLS_MD_SHA256, ...)` (mbedtls, already linked for HKDF).
- Consumes: `E2EKeyStore` per-peer `k_up` accessor. Verify the exact API at implementation time — likely `e2eKeys.lookupUp(mac, out)` or `derivePeerKeys(mac, kUp, kDown)`. Use the one that returns the master-side upstream key for a given node.
- Produces: `lattice::mesh::routemac::buildHopContext(msg, prev_hop, this_hop, out_ctx)` (void).
- Produces: `lattice::mesh::routemac::chainStep(secret, hop_ctx, prev_mac, out_mac)` (void).

- [ ] **Step 1: Bump submodule to v0.5.0**

```bash
cd /Users/benji/projects/personal/lattice-nodes
git checkout main
git pull --ff-only
git checkout -b feat/phaseC-downlink-auth
cd firmware/main/lib/lattice-protocol
git fetch --tags
git checkout v0.5.0
cd ../../..
git add firmware/main/lib/lattice-protocol
```

Verify `firmware/main/lib/lattice-protocol/c/mesh_message.h` now has `uint8_t auth_path[8];` and `static_assert(sizeof(mesh_message) == 250, ...)`.

- [ ] **Step 2: Write the failing RouteMac unit tests**

Create `tests/unit/test_route_mac.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "mesh/RouteMac.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

using namespace lattice::mesh::routemac;

static mesh_message makeMsg() {
  mesh_message m{};
  uint8_t origin[6]  = {0xAA,0,0,0,0,1};
  uint8_t target[6]  = {0xBB,0,0,0,0,2};
  memcpy(m.origin_mac_address, origin, 6);
  memcpy(m.target_mac_address, target, 6);
  m.epoch_num = 0x11223344;
  m.seq_num   = 0x5566;
  return m;
}

TEST(RouteMac, BuildHopContext_ByteExact) {
  mesh_message m = makeMsg();
  uint8_t prev[6] = {0x01,0x02,0x03,0x04,0x05,0x06};
  uint8_t self[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
  uint8_t ctx[HOP_CTX_LEN];
  buildHopContext(m, prev, self, ctx);
  // origin(6) || dest(6) || epoch LE(4) || seq LE(2) || prev(6) || self(6)
  const uint8_t want[HOP_CTX_LEN] = {
    0xAA,0,0,0,0,1,            // origin
    0xBB,0,0,0,0,2,            // dest
    0x44,0x33,0x22,0x11,       // epoch LE
    0x66,0x55,                 // seq LE
    0x01,0x02,0x03,0x04,0x05,0x06,  // prev
    0x11,0x22,0x33,0x44,0x55,0x66   // self
  };
  ASSERT_EQ(0, memcmp(ctx, want, HOP_CTX_LEN));
}

TEST(RouteMac, ChainStep_StableForSameInput) {
  uint8_t secret[32]; for (int i=0; i<32; ++i) secret[i] = i;
  uint8_t ctx[HOP_CTX_LEN]; for (size_t i=0; i<HOP_CTX_LEN; ++i) ctx[i] = i;
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx, prev, out1);
  chainStep(secret, ctx, prev, out2);
  ASSERT_EQ(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenSecretChanges) {
  uint8_t s1[32] = {0}; s1[0] = 1;
  uint8_t s2[32] = {0}; s2[0] = 2;
  uint8_t ctx[HOP_CTX_LEN] = {0};
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(s1, ctx, prev, out1);
  chainStep(s2, ctx, prev, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenContextChanges) {
  uint8_t secret[32] = {0};
  uint8_t ctx1[HOP_CTX_LEN] = {0}; ctx1[0] = 1;
  uint8_t ctx2[HOP_CTX_LEN] = {0}; ctx2[0] = 2;
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx1, prev, out1);
  chainStep(secret, ctx2, prev, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenPrevMacChanges) {
  uint8_t secret[32] = {0};
  uint8_t ctx[HOP_CTX_LEN] = {0};
  uint8_t p1[AUTH_PATH_LEN] = {0};
  uint8_t p2[AUTH_PATH_LEN] = {0}; p2[0] = 1;
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx, p1, out1);
  chainStep(secret, ctx, p2, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}
```

- [ ] **Step 3: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_route_mac --parallel
ctest --test-dir tests/build -R RouteMac --output-on-failure
```

Expected: compile error — `RouteMac.h` doesn't exist. Also `tests/unit/CMakeLists.txt` may need `add_executable(test_route_mac ...)` added.

- [ ] **Step 4: Implement `RouteMac.h`**

Create `firmware/main/src/mesh/RouteMac.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/md.h>
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {
namespace routemac {

constexpr size_t HOP_CTX_LEN = 30;
constexpr size_t AUTH_PATH_LEN = 8;

inline void buildHopContext(const mesh_message& msg, const uint8_t prev_hop[6],
                            const uint8_t this_hop[6], uint8_t out_ctx[HOP_CTX_LEN]) {
  uint8_t* p = out_ctx;
  memcpy(p, msg.origin_mac_address, 6); p += 6;
  memcpy(p, msg.target_mac_address, 6); p += 6;
  p[0] = static_cast<uint8_t>(msg.epoch_num);
  p[1] = static_cast<uint8_t>(msg.epoch_num >> 8);
  p[2] = static_cast<uint8_t>(msg.epoch_num >> 16);
  p[3] = static_cast<uint8_t>(msg.epoch_num >> 24);
  p += 4;
  p[0] = static_cast<uint8_t>(msg.seq_num);
  p[1] = static_cast<uint8_t>(msg.seq_num >> 8);
  p += 2;
  memcpy(p, prev_hop, 6); p += 6;
  memcpy(p, this_hop, 6);
}

inline void chainStep(const uint8_t secret[32], const uint8_t hop_ctx[HOP_CTX_LEN],
                      const uint8_t prev_mac[AUTH_PATH_LEN], uint8_t out_mac[AUTH_PATH_LEN]) {
  uint8_t input[HOP_CTX_LEN + AUTH_PATH_LEN];
  memcpy(input, hop_ctx, HOP_CTX_LEN);
  memcpy(input + HOP_CTX_LEN, prev_mac, AUTH_PATH_LEN);

  uint8_t full[32];  // SHA-256 output
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, secret, 32, input, sizeof(input), full);
  memcpy(out_mac, full, AUTH_PATH_LEN);   // truncate to first 8 bytes
}

} // namespace routemac
} // namespace mesh
} // namespace lattice
```

If `tests/unit/CMakeLists.txt` uses a glob or an explicit list, add `test_route_mac.cpp` accordingly.

- [ ] **Step 5: Run RouteMac tests to verify pass**

```bash
cmake --build tests/build --target test_route_mac --parallel
ctest --test-dir tests/build -R RouteMac --output-on-failure
```

Expected: 5 PASS.

- [ ] **Step 6: Locate the OpRouteReport originate + relay paths + master verify**

```bash
grep -n "route_path\|route_len\|OpRouteReport\|processRouteReport" firmware/main/src/mesh/Mesh.cpp
```

Identify three edit points:
- (a) the node's own OpRouteReport originate site (builds the frame with itself as first hop);
- (b) the relay-forward site that rewrites `route_path` on forward (appends this relay's MAC, increments `route_len`);
- (c) `Mesh::processRouteReport` (master side, records into `RouteTable`).

Verify by reading each: (a) sets `msg.route_len = 1`, `msg.route_path[0..6] = deviceMacAddress`; (b) appends `deviceMacAddress` at `msg.route_path[msg.route_len * 6]`, `msg.route_len++`; (c) exists and calls `routes->record(...)`.

- [ ] **Step 7: Wire accumulation at originate (edit point (a))**

Before send, after `route_path[0..6]`/`route_len=1` are set:

```cpp
uint8_t k_up[32];
if (!e2eKeys.lookupUp(masterMac, k_up)) {   // master's MAC = knownMasterMac / currentMaster.mac
  Logger::logln("MESH", "Route report: no k_up for master, dropping", LogLevel::LOG_WARN);
  return;
}
uint8_t ctx[routemac::HOP_CTX_LEN];
uint8_t prev_hop[6] = {0};
routemac::buildHopContext(msg, prev_hop, deviceMacAddress, ctx);
uint8_t prev_mac[routemac::AUTH_PATH_LEN] = {0};
routemac::chainStep(k_up, ctx, prev_mac, msg.auth_path);
```

- [ ] **Step 8: Wire accumulation at relay (edit point (b))**

Before appending this relay's MAC to `route_path`, snapshot `prev_hop = msg.route_path[(route_len-1)*6..6]` (the previous last hop). Then append this relay's MAC and increment `route_len`. Then compute the chain step:

```cpp
uint8_t prev_hop[6];
memcpy(prev_hop, &msg.route_path[(msg.route_len - 1) * 6], 6);   // BEFORE append
memcpy(&msg.route_path[msg.route_len * 6], deviceMacAddress, 6);  // append
msg.route_len++;

uint8_t k_up[32];
if (!e2eKeys.lookupUp(masterMac, k_up)) return;   // silent drop; can't authenticate
uint8_t ctx[routemac::HOP_CTX_LEN];
routemac::buildHopContext(msg, prev_hop, deviceMacAddress, ctx);
uint8_t prev_mac[routemac::AUTH_PATH_LEN];
memcpy(prev_mac, msg.auth_path, routemac::AUTH_PATH_LEN);
routemac::chainStep(k_up, ctx, prev_mac, msg.auth_path);
```

- [ ] **Step 9: Wire master verification (edit point (c))**

At the top of `Mesh::processRouteReport`, before the existing `routes->record(...)` call:

```cpp
uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
uint8_t prev_hop[6] = {0};
for (size_t i = 0; i < msg.route_len; ++i) {
  const uint8_t* hop_mac = &msg.route_path[i * 6];
  uint8_t k_up[32];
  if (!e2eKeys.lookupUp(hop_mac, k_up)) {
    Logger::logln("MESH", "Route report: unknown hop; drop", LogLevel::LOG_ERROR);
    return;
  }
  uint8_t ctx[routemac::HOP_CTX_LEN];
  routemac::buildHopContext(msg, prev_hop, hop_mac, ctx);
  routemac::chainStep(k_up, ctx, computed, computed);
  memcpy(prev_hop, hop_mac, 6);
}
if (memcmp(computed, msg.auth_path, routemac::AUTH_PATH_LEN) != 0) {
  Logger::logln("MESH", "Route report: MAC verify failed; drop", LogLevel::LOG_ERROR);
  return;
}
```

- [ ] **Step 10: Extend `test_route_report.cpp` with 4 verify cases**

```cpp
TEST_F(RouteReportTest, MasterVerifiesValidChain_RecordsPath) {
  // Set up master + 2 known hops with k_up seeded in e2eKeys.
  // Build a valid 2-hop route_report frame using RouteMac helpers.
  // Call processRouteReport, assert routes->lookup returns the recorded path.
}

TEST_F(RouteReportTest, MasterRejectsTamperedRoutePath_NoRecord) {
  // Build valid frame, flip one byte in route_path AFTER MAC-ing.
  // processRouteReport, assert routes->lookup returns false (no record).
}

TEST_F(RouteReportTest, MasterRejectsTamperedAuthPath_NoRecord) {
  // Build valid frame, flip one byte in auth_path.
  // processRouteReport, assert routes->lookup returns false.
}

TEST_F(RouteReportTest, MasterRejectsUnknownHop_NoRecord) {
  // Build frame with a hop MAC that has no entry in e2eKeys.
  // processRouteReport, assert routes->lookup returns false.
}
```

- [ ] **Step 11: Extend `test_route_report_e2e.cpp` with 2 scenarios**

`RouteReportEndToEnd_ValidChain` — sim network with master + 2 relays + leaf; leaf originates uplink, relays accumulate + MAC, master verifies + records; assert `master.routes->lookup(leaf_mac, ...)` returns the correct path.

`RouteReportEndToEnd_TamperedByEvilRelay` — same, but a middle relay flips one byte of `auth_path` before forwarding; master drops; `master.routes->lookup(leaf_mac, ...)` returns false.

- [ ] **Step 12: Run full suite for regressions**

```bash
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
ctest --test-dir tests/build --output-on-failure --parallel 4 -L e2e -R "route"
```

Expected: all PASS.

- [ ] **Step 13: Commit + push + open PR**

```bash
git add firmware/main/lib/lattice-protocol \
        firmware/main/src/mesh/RouteMac.h \
        firmware/main/src/mesh/Mesh.cpp \
        firmware/main/CMakeLists.txt \
        tests/unit/CMakeLists.txt \
        tests/unit/test_route_mac.cpp \
        tests/unit/test_route_report.cpp \
        tests/e2e/scenarios/test_route_report_e2e.cpp
git commit -m "feat(mesh): chain-MAC route_report accumulation + master verify (closes #44)

Adds RouteMac.h: HMAC-SHA256-64 chained per hop over
origin||dest||epoch||seq||prev_hop||this_hop, keyed off the reporting
node's pairwise k_up with the master. Each relay chains its hop into
msg.auth_path as it appends to route_path; the master reconstructs
from k_up for each hop and drops+logs on mismatch. RouteTable is
recorded only on verify pass.

No new persisted state; no server participation; robust to hot-swap
(new node's fresh enrollment yields a valid k_up immediately).

Bumps lattice-protocol submodule to v0.5.0 (protocol v4)."
git push -u origin feat/phaseC-downlink-auth
gh pr create --title "feat(phaseC): chain-MAC route auth (closes #44)" \
             --body "..."
```

---

### Task 3: Hub — bump protocol dep + ProtoVersion gate (lattice-hub)

**Repo:** `/Users/benji/projects/personal/lattice-hub` — branch `chore/phaseC-protocol-v0.5.0`.

**Precondition:** protocol `v0.5.0` tag exists on origin.

**Files:**
- Modify: `server/orchestrator/go.mod`, `go.sum` — bump `lattice-protocol` to v0.5.0.
- Regen: `server/orchestrator/mesh/mesh.pb.go` (via `go generate ./mesh/...`).
- Modify: `server/orchestrator/mesh/server.go` — `ProtoVersion` gate: `== 3` → `== 4`.

**Interfaces:**
- Consumes: protocol v0.5.0 `MeshMessage.AuthPath` (opaque bytes to hub).
- Produces: nothing new. Hub does not verify.

- [ ] **Step 1: Sync + branch**

```bash
cd /Users/benji/projects/personal/lattice-hub
git checkout main
git pull --ff-only
git checkout -b chore/phaseC-protocol-v0.5.0
```

- [ ] **Step 2: Bump go.mod + tidy**

```bash
cd server/orchestrator
go get github.com/superbrobenji/lattice-protocol@v0.5.0
go mod tidy
```

Verify: `grep lattice-protocol go.mod` shows `v0.5.0`.

- [ ] **Step 3: Regenerate mesh.pb.go**

```bash
go generate ./mesh/...
```

Verify: `mesh/mesh.pb.go` now has `AuthPath []byte` field (proto3 tag 17).

- [ ] **Step 4: Bump ProtoVersion gate**

```bash
grep -n "ProtoVersion != 3\|ProtoVersion == 3\|ProtoVersion: 3\|ProtoVersion:  *3" mesh/*.go
```

Update every hit from `3` → `4`. Typically there's one gate in `server.go` around inbound message handling; there may also be several outbound builders that set `ProtoVersion: 3` — bump all.

- [ ] **Step 5: Test**

```bash
go test -race -count=1 ./...
```

Expected: all pass. If any test hardcoded `ProtoVersion: 3` in an inbound-frame fixture, update to `4`.

- [ ] **Step 6: Commit + push + open PR**

```bash
git add server/orchestrator/go.mod server/orchestrator/go.sum \
        server/orchestrator/mesh/mesh.pb.go \
        server/orchestrator/mesh/server.go
# plus any test files that needed the 3→4 fixture bump
git commit -m "chore(deps): bump lattice-protocol to v0.5.0 (protocol v4 flag-day)

v0.5.0 adds AuthPath[8] to MeshMessage (chained HMAC-SHA256-64 over
route_path, firmware-authored, master-verified). Hub does not
participate in path authentication — AuthPath is opaque bytes on the
hub-side, forwarded verbatim.

Bumps ProtoVersion gate from 3 to 4 atomically per flag-day rule.
Old firmware speaking v3 will be rejected on the next merge — that
is intentional."
git push -u origin chore/phaseC-protocol-v0.5.0
gh pr create --title "chore(deps): bump lattice-protocol to v0.5.0 (protocol v4 flag-day)" \
             --body "..."
```

---

### Task 4: Cross-repo verify + close #44

**Repo:** verification-only; no code changes.

- [ ] **Step 1: Confirm all three PRs merged in order** — protocol first (already tagged), then nodes + hub.
- [ ] **Step 2: Run nodes full suite one more time on the merged main**
- [ ] **Step 3: Confirm `#44` auto-closed** by the nodes PR's `Closes #44` line.
- [ ] **Step 4: Write SDD ledger completion line** at `.superpowers/sdd/phaseC-downlink-auth/progress.md`.

---

## Self-review

**Spec coverage:**
- §Design/1 (protocol wire) → Task 1.
- §Design/2 (nodes MAC + verify) → Task 2.
- §Design/3 (hub dep bump + gate) → Task 3.
- §Cross-repo release-flow → Task ordering (1 must complete tag push before 2/3 open).
- §Testing (protocol codegen, nodes unit + e2e, hub proto-sync) → Task 1 Step 2, Task 2 Steps 2/5/10/11/12, Task 3 Step 5.
- §Non-goals → respected: no downlink-frame MAC, no hub verification, no persisted state, no v3 backcompat.

**Type consistency:**
- `MeshMessage.AuthPath [8]byte` — declared Task 1 Step 1, consumed by Task 2 (via submodule regen).
- `routemac::HOP_CTX_LEN = 30`, `AUTH_PATH_LEN = 8` — declared Task 2 Step 4, consumed Task 2 Steps 7/8/9.
- `routemac::buildHopContext(msg, prev, self, out_ctx)` — declared Task 2 Step 4, consumed Task 2 Steps 7/8/9 and test Step 2.
- `routemac::chainStep(secret, ctx, prev_mac, out_mac)` — declared Task 2 Step 4, consumed same.
- `e2eKeys.lookupUp(mac, out_key)` — VERIFY at implementation time; the plan uses this name as a placeholder and flags it explicitly in Task 2's Interfaces block.
- `ProtoVersion` gate `!= 4` — Task 3 Step 4.

**Placeholder scan:**
- Task 2 Interfaces says "verify the exact API at implementation time — likely `e2eKeys.lookupUp(mac, out)` or `derivePeerKeys(mac, kUp, kDown)`" — this is a legitimate implementer-time verification, not a hidden TODO. The Step 7/8/9 code samples use `e2eKeys.lookupUp(...)` consistently; the implementer must adapt to the real API name if it differs. This is called out in the Interfaces block explicitly.
- Task 3 Step 6 PR body is a placeholder `"..."`; the implementer expands it. Not a defect — brief structure is intentional.

**Scope check:** 4 tasks across 3 repos, one canonical design doc, one plan doc. Cross-repo release-flow rule is enforced by task ordering + explicit precondition lines. Appropriate for one plan.
