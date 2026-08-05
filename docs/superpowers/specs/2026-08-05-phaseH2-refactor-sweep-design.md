# Phase H2 — Refactor sweep (audit items R-AA)

**Status:** Approved
**Date:** 2026-08-05
**Repo:** lattice-nodes only. No cross-repo, no wire changes.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase H2).
**Audit source:** `docs/superpowers/specs/2026-08-04-post-phaseG-audit-findings.md` items R-AA.

## Context

Post-Phase-G audit surfaced 10 medium-scope DRY/OOP/pattern refactors. Bundled as Phase H2. No upstream issue; audit-driven follow-through. All firmware-only.

## Design

### R — `String`-concat elimination on hot paths

**Where:** **23 sites** (verified 2026-08-05) `LATTICE_LOG*(...String(...)...)` across `mesh/`, `adapter/`, `hardware/`. The pre-audit "78 sites" estimate covered ALL `String(` constructions repo-wide; the log-specific hot-path subset is 23.
**Fix:** replace `String("x") + String(v)` args passed to `LATTICE_LOG*` with `snprintf(buf, sizeof(buf), "x %d", v)` into stack `char[N]`. At production `LOG_NONE` the macro folds and String temporaries drop out of the build entirely (verified via post-Phase-G size run — no extra flash to reclaim there). Value is: belt-and-braces for any future non-NONE build, plus cleaner call sites.
**Also survey:** high-frequency non-log String constructions if any surface during the migration — grep `String(` outside `LATTICE_LOG*` and check hot-path callers.
**Est:** removes heap churn on non-NONE builds; ~0.5-1 KB flash from dropped String inlining under non-NONE.

### S — `DisplayManager::tick` throttle

**Where:** `firmware/main/src/app/DisplayManager.h:22-27`.
**Fix:** track `_lastValue` + `_lastNodeId`; call `display.show(...)` only when value or nodeId changed. Keep 500 ms toggle timer for pre-enroll blink state.
**Est:** ~100-200 ms/sec CPU reclaimed.

### T — Non-blocking `Button::isPressed`

**Where:** `firmware/main/src/hardware/input/Button.cpp:26-35`.
**Fix:** replace `delay(5)*2 = 10 ms` blocking with rolling debounce vote — sample every N ms via `millis()` timestamp; return "pressed" only after M consecutive positive samples within window. Non-blocking.
**Est:** removes ~20 ms/loop stall; unblocks tickless idle (Phase I item EE).

### U — `sendBroadcast` helper

**Where:** **6 sites** (verified 2026-08-05) of `esp_now_send(BROADCAST_MAC, ...)` across `Mesh.cpp` + `Enrollment.cpp`. Phase G Task 2 item E consolidated the MAC constant into `broadcast_mac.h` but not the send call.
**Fix:** `bool Mesh::sendBroadcast(const mesh_message& msg)` wraps `esp_now_send(BROADCAST_MAC, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg))` + error handling. Callers become one-liners.
**Est:** ~150-250 B flash (6 sites × ~30-40 B each).

### V — Adapter control-op dispatch table

**Where:** `Adapter.cpp:41-93` vs `SerialAdapter.cpp:105-129, 312-405`. OP_CONFIG_SET / OP_NODE_ID_SET / OP_HEALTH_REQ / OP_TX_POWER_SET duplicated between base `Adapter::onMeshData` and `SerialAdapter::onMeshDataImpl` (SerialAdapter also handles them in `handleCompleteFrame`).
**Fix:** static dispatch table `{opcode → handler_fn}` shared by both entry points. Handlers become small free functions or Adapter static methods. Removes ~150 lines duplicate.
**Est:** ~1-2 KB flash.

### W — Health-frame builder in `Adapter` base

**Where:** `PirAdapter.cpp:72-86` vs `SerialAdapter.cpp:24-54`. `sendNodeHealth`/`sendHealthReport` build the same shape (opcode + adapterType + 6B MAC + 4B LE uptime → `data[64]`).
**Fix:** `Adapter::buildHealthFrame(mesh_message& out)` on base; PIR + Serial call it. Also move the `_lastHealthMillis` interval poll to base (identical in both).
**Est:** ~300-500 B flash + ~10 B RAM (one interval field on base).

### X — Fold `neighbors.observe + minFreshDistance`

**Where:** `Mesh.cpp:843, 850` (both in `processMasterBeacon`).
**Fix:** either (a) return the new min from `observe(mac, dist, now)` after inserting; or (b) add `NeighborTable::observeAndMinDistance(mac, dist, now)` doing both in one pass. Currently two full linear scans of the neighbor table per beacon RX.
**Est:** halves neighbor-scan cost per beacon.

### Y — Shared MAC-keyed-table helpers

**Where:** 5 classes reimplement the "linear-scan-by-MAC + slot-allocate + `memcpy(entry.mac, mac, 6); valid=true;`" skeleton: `NeighborTable`, `RouteTable`, `E2EKeyStore`, `ReplayCache`, `PeerRegistry`. ~180 lines duplicated. **Complementary to Phase G item Q** which added `lattice::mac::eq(a, b)` — that fixed the compare idiom; Y fixes the enclosing table skeleton.
**Fix:** free-function helpers (NOT templated, to avoid bloat):
```cpp
namespace lattice::mac_table {
  // stride = entry byte size; mac_offset = MAC field offset in each entry.
  // Returns index or SIZE_MAX if not found.
  size_t find(const void* entries, size_t n, size_t stride, size_t mac_offset, const uint8_t mac[6]);
  size_t evict_oldest_by_ts(const void* entries, size_t n, size_t stride, size_t ts_offset);
}
```
Each class thins its `findSlot`/`allocateSlot` to a one-liner call.
**Est:** ~100-150 lines dedup, ~0.8-1.2 KB flash.

### Z — `is_zero` helper

**Where:** `Enrollment.cpp:158`, `E2EKeyStore.h:42`, `Mesh.cpp:1102`. Three hand-rolled zero-check loops (6B and 32B).
**Fix:** `bool lattice::mem::is_zero(const uint8_t* p, size_t n)` in `network/mem.h` or similar. Trivial.
**Est:** tiny flash + clarity.

### AA — Singleton → namespace migration

**Where:** **39 `getInstance()` sites** (verified 2026-08-05) across `EepromManager`, `Mesh`, `PirAdapter`, `ErrorCore`.
**Fix:** replace Meyers singletons with `namespace lattice::eeprom { ... }` free functions backed by file-static state. Each call site drops `__cxa_guard_*` prologue (~40 B + byte flag) per unique callsite.
**Est:** ~0.5-1 KB flash + shorter callsites.

**Scope caveat for AA:** `Mesh::instance` is used by static trampolines (`dataRecvTrampoline`, `registerPeerWithKeyTrampoline` etc.) that live inside class scope. Migrating away from `Mesh::getInstance()` requires either keeping the class or migrating trampolines to free functions with file-static access to a `Mesh*`. Non-trivial. Recommend deferring `Mesh` migration — do `EepromManager`, `PirAdapter`, `ErrorCore` only. `Mesh` singleton stays.

**Additional note (2026-08-05):** grep counts confirm 39 total across all four singletons; the `Mesh` share is dominant. Deferring it keeps the item bounded to `EepromManager` (~25 sites), `PirAdapter` (~4), `ErrorCore` (~3) — realistically 30+ sites still tackled.

## Non-goals

- No wire changes.
- No hub / protocol touches.
- No Phase I items (native ESP-IDF leverage — separate phase).
- No new features.

## Testing

- Full unit suite green after each item.
- Full e2e suite green after item T (Button debounce), item V (dispatch table), item W (health-frame dedup).
- CI size delta reported in PR body.

## Files touched (estimate)

- Multiple `Logger` call sites (item R): grep + rewrite ~30-40 sites.
- `DisplayManager.h`: +tracking fields, +conditional show.
- `Button.{h,cpp}`: rolling-vote debounce.
- `Mesh.{h,cpp}`: `sendBroadcast` helper; `neighbors.observe` fold.
- `NeighborTable.h`: `observeAndMinDistance` variant.
- `Adapter.{h,cpp}` + `PirAdapter.cpp` + `SerialAdapter.cpp`: op-dispatch table, health-frame base helper.
- `mac_table.h` (new), `mem.h` (new).
- 5 mesh classes (NeighborTable, RouteTable, E2EKeyStore, ReplayCache, PeerRegistry): thin `findSlot`/`allocateSlot` via `mac_table::` helpers.
- `EepromManager.{h,cpp}`, `PirAdapter.{h,cpp}`, `ErrorCore.{h,cpp}`: singleton → namespace.

Rough size: ~250 LOC production + ~50 LOC test.

## Est. impact

~1-2 KB flash + hot-path heap-churn elimination + CPU reclamation (items S, T, X) that indirectly unblocks Phase I's tickless-idle savings.
