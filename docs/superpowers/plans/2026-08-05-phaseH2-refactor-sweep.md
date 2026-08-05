# Phase H2 — Refactor sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Land all 10 audit R-AA items per `docs/superpowers/specs/2026-08-05-phaseH2-refactor-sweep-design.md`. Firmware-only, no cross-repo, no wire changes.

**Architecture:** 6 nodes-only tasks. Each is a coherent bundle with its own PR-worthy diff. Tasks are independent — no ordering constraint beyond each running on latest main.

## Global Constraints

- No wire-format changes.
- Firmware-only. No `lattice-protocol` or `lattice-hub` touches.
- Tiger-Style — no dynamic alloc on hot paths.
- Under `UNIT_TEST`, `err::fail` throws `FatalError`.
- Design canonical: `docs/superpowers/specs/2026-08-05-phaseH2-refactor-sweep-design.md`.
- Audit ledger: `docs/superpowers/specs/2026-08-04-post-phaseG-audit-findings.md`.

---

### Task 1: R — `String`-concat elimination

**Files:**
- Modify: ~23 sites of `LATTICE_LOG*(...String(...)...)` across `firmware/main/src/mesh/`, `.../adapter/`, `.../hardware/`.

**Approach:** grep `LATTICE_LOG.*String(` to enumerate. Rewrite each into a stack `char buf[N]; snprintf(buf, sizeof(buf), "fmt", args...); LATTICE_LOGLN(tag, buf, level);` pattern. Choose N per site (typical 64-128).

- [ ] **Step 1:** Enumerate + rewrite mechanically.
- [ ] **Step 2:** Full unit + e2e regression.
- [ ] **Step 3:** Commit + push + PR:

```
fix(mesh+adapter+hardware): String → snprintf at LATTICE_LOG* sites (audit R)

Even under production LOG_NONE (macros fold to void), the String args
were still built as inputs. Replaces the 23 sites of
LATTICE_LOG*(...String(...)...) with stack-buf snprintf so String
temporaries drop out of the build entirely.
```

---

### Task 2: S + T — DisplayManager throttle + Button non-blocking

**Files:**
- Modify: `firmware/main/src/app/DisplayManager.h` — add `_lastValue`, `_lastNodeId`; gate `display.show(...)` on value change.
- Modify: `firmware/main/src/hardware/input/Button.{h,cpp}` — rolling-vote debounce, non-blocking.
- Test: extend or add fixtures in `tests/unit/test_mesh_logic.cpp` (or a new small test file if `DisplayManager`/`Button` don't have coverage yet).

**Interfaces:**
- `Button::isPressed()` signature unchanged; internal state gains `_lastPollMs` + `_debounceHistory` (small ring or bitfield).
- `DisplayManager::tick()` signature unchanged.

- [ ] **Step 1:** DisplayManager change-detection.
- [ ] **Step 2:** Button rolling-vote debounce (sample every ~5 ms; require ≥3 consecutive matching samples in a 20 ms window).
- [ ] **Step 3:** Test regression.
- [ ] **Step 4:** Commit + push + PR.

---

### Task 3: U + X — `sendBroadcast` helper + neighbors observe+min fold

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.{h,cpp}` — add `bool sendBroadcast(const mesh_message& msg)`; replace 6 `esp_now_send(BROADCAST_MAC, ...)` sites.
- Modify: `firmware/main/src/mesh/NeighborTable.h` — add `uint8_t observeAndMinDistance(mac, dist, now)` that inserts and returns the new min in one pass.
- Modify: `firmware/main/src/mesh/Mesh.cpp::processMasterBeacon` (~lines 843, 850) — call new fold instead of `observe(...)` + `minFreshDistance(...)`.

- [ ] **Step 1:** `sendBroadcast` helper + replace 6 sites.
- [ ] **Step 2:** `NeighborTable::observeAndMinDistance`; keep existing `observe`/`minFreshDistance` for other callers.
- [ ] **Step 3:** `processMasterBeacon` migration.
- [ ] **Step 4:** Regression + commit + push + PR.

---

### Task 4: V + W — Adapter op-dispatch table + health-frame base

**Files:**
- Modify: `firmware/main/src/adapter/Adapter.{h,cpp}` — add static dispatch table `{opcode → handler}`; add `buildHealthFrame(mesh_message& out)`; add `_lastHealthMillis` interval poll.
- Modify: `firmware/main/src/adapter/serial/SerialAdapter.cpp` — remove duplicate OP_CONFIG_SET/OP_NODE_ID_SET/OP_HEALTH_REQ/OP_TX_POWER_SET handlers; delegate to dispatch table. Remove local `sendHealthReport` in favor of base.
- Modify: `firmware/main/src/adapter/pir/PirAdapter.cpp` — remove local `sendNodeHealth`; call base helper.

**Interfaces:**
- `Adapter::onMeshData` (or the base entry point) dispatches to the shared table.
- `Adapter::buildHealthFrame(mesh_message& out)` — new base method.

- [ ] **Step 1:** Extract handlers into free functions or static Adapter methods; build dispatch table.
- [ ] **Step 2:** Base `buildHealthFrame` + interval tick.
- [ ] **Step 3:** Migrate PIR + Serial.
- [ ] **Step 4:** Regression + commit + push + PR.

---

### Task 5: Y + Z — Shared `mac_table` + `is_zero` helpers

**Files:**
- Create: `firmware/main/src/network/mac_table.h` — free-function helpers `find(entries, n, stride, mac_offset, mac)` + `evict_oldest_by_ts(entries, n, stride, ts_offset)`.
- Create: `firmware/main/src/network/mem.h` — `is_zero(const uint8_t*, size_t)`.
- Modify: `NeighborTable.h`, `RouteTable.h`, `E2EKeyStore.h`, `ReplayCache.h`, `PeerRegistry.cpp` — thin `findSlot`/`allocateSlot` via `mac_table::` helpers.
- Modify: `Enrollment.cpp:158`, `E2EKeyStore.h:42`, `Mesh.cpp:1102` — use `mem::is_zero`.

**Interfaces:**
- `size_t lattice::mac_table::find(const void*, size_t n, size_t stride, size_t mac_offset, const uint8_t mac[6])` — returns index or `SIZE_MAX`.
- `size_t lattice::mac_table::evict_oldest_by_ts(const void*, size_t n, size_t stride, size_t ts_offset)`.
- `bool lattice::mem::is_zero(const uint8_t*, size_t)`.

- [ ] **Step 1:** Write helpers with unit tests.
- [ ] **Step 2:** Thin one class at a time (5 iterations); regression per class.
- [ ] **Step 3:** Replace 3 `is_zero` sites.
- [ ] **Step 4:** Full regression + commit + push + PR.

---

### Task 6: AA — Singleton → namespace (EepromManager, PirAdapter, ErrorCore only)

**Files:**
- Modify: `firmware/main/src/persistence/EepromManager.{h,cpp}` — replace class + `getInstance()` with `namespace lattice::eeprom { ... }` free functions holding file-static state (`Preferences _prefs`, `bool _isDevMode`, `uint32_t _devEpoch`). Public API preserved: same function names, no class prefix.
- Modify: `firmware/main/src/adapter/pir/PirAdapter.{h,cpp}` — same treatment; keep `PirAdapter` as a class only if the `Adapter` polymorphic hierarchy requires it (verify at impl time — likely does; then only migrate away the `getInstance()` sites that aren't part of the polymorphic interface).
- Modify: `firmware/main/src/error/ErrorCore.{h,cpp}` — same.
- Modify: all ~30-35 caller sites — remove `getInstance()` prefix.

**Interfaces:**
- `lattice::eeprom::loadBootEpoch()` etc. — flat namespace.
- `Mesh::getInstance()` retained — trampolines depend on it (deferred per design caveat).

- [ ] **Step 1:** EepromManager migration (biggest — ~25 sites).
- [ ] **Step 2:** Verify tests pass + regression.
- [ ] **Step 3:** ErrorCore migration.
- [ ] **Step 4:** PirAdapter migration (keep class for polymorphism; only migrate `getInstance()` if used outside the base-class dispatch).
- [ ] **Step 5:** Full regression + commit + push + PR.

---

### Task 7: verify + PR

- [ ] All 6 PRs (Tasks 1-6) queued; merge in order (each rebases on the previous).
- [ ] CI size delta reported on the final PR body.
- [ ] SDD ledger.
- [ ] No follow-up issue to close — Phase H2 is audit-driven with no upstream tracker.

---

## Self-review

**Coverage:**
- R → Task 1
- S + T → Task 2
- U + X → Task 3
- V + W → Task 4
- Y + Z → Task 5
- AA → Task 6

**Type consistency:**
- `sendBroadcast(const mesh_message&) → bool` — Mesh method.
- `observeAndMinDistance(mac, dist, now) → uint8_t` — NeighborTable method.
- `mac_table::find(...) → size_t` (SIZE_MAX sentinel), `mac_table::evict_oldest_by_ts(...) → size_t`.
- `mem::is_zero(const uint8_t*, size_t) → bool`.

**Placeholder scan:** none. Every task has concrete file:site targets.

**Scope check:** 6 tasks, all firmware-only, all independent — appropriate for one plan across multiple sequenced PRs.
