# Phase E — Array-in-Interface Consistency Design

**Status:** Approved, ready for writing-plans.
**Date:** 2026-08-11
**Parent:** `docs/superpowers/specs/2026-08-07-clean-code-refactor-umbrella-design.md` (Phase E)
**Source:** CodeQL `cpp/array-in-interface` alerts surfaced on Phase B's PR (#97), not from the
original Phase A audit. Umbrella spec required this phase to either justify why `std::array`
parameters differ from Phase A finding 7's rejected `MacAddress` wrapper, or conclude the alerts
should stay dismissed.

## Current State (verified 2026-08-11)

7 live `cpp/array-in-interface` alerts, all pre-existing/dismissed-or-open, none new:
- 6 dismissed `won't fix` from PR #97: `MasterBeacon.cpp:35,52`, `DownlinkRouter.cpp:17`,
  `Mesh.cpp:134`, `Mesh.h:196`, `MeshTransport.cpp:192`.
- 1 open, pre-dating this umbrella entirely: `Adapter.cpp:32`.

## Decision

**Neither of the two options the umbrella spec anticipated.** Adopting `std::array<uint8_t,6>`
would require touching the same ~15+ call sites for a container-class wrapper Phase A finding 7
already rejected as "strictly worse" than raw-byte handling (extra temporary-object copies, no
behavioral gain — every mesh table already stores raw `uint8_t[6]` and compares via
`lattice::mac::eq`). Leaving the alerts dismissed doesn't actually fix anything.

**Third option: rewrite the array-bracket parameter syntax to plain pointer syntax.**
`void f(uint8_t x[6])` and `void f(uint8_t* x)` are identical at the type/ABI level — a C++ array
parameter always decays to a pointer; the `[6]` never carried real bounds information the compiler
enforces. CodeQL's `cpp/array-in-interface` query matches the AST's array-type declarator
specifically, not plain pointer parameters — so this change:
- Resolves every alert for real, not by dismissal.
- Requires **zero call-site changes anywhere** — decay is identical either way, confirmed against
  every caller including ESP-IDF's own `esp_wifi_get_mac(wifi_interface_t, uint8_t mac[6])`, which
  `hw_mac.h`'s `readOwnMac` calls directly with the same buffer either way.
- Does not reintroduce finding 7's rejected wrapper pattern — this is honest syntax for what was
  always a pointer, not a new type.
- Zero behavior change, zero wire change, zero risk.

## Scope — 17 functions across 9 files

Grep-verified against the current tree (`firmware/main/src`), filtered to actual function
parameters (struct fields and local buffers are unaffected — this rule doesn't apply to them and
they should not change):

| File(s) | Function(s) | Notes |
|---|---|---|
| `mesh/DownlinkRouter.h` + `.cpp` | `classify(...)` | `uint8_t nextHopMacOut[6]` → `uint8_t* nextHopMacOut` |
| `mesh/Mesh.h` + `.cpp` | `handleReceivedMessage`, `handleReceivedMessageTrampoline` | both take `const uint8_t srcMac[6]`; must change together with `MeshTransport::MessageHandler` (below), since the trampoline is bound to that typedef |
| `mesh/MeshTransport.h` + `.cpp` | `MessageHandler` typedef, `registerPeerWithEspNow` | `MessageHandler = void (*)(const uint8_t srcMac[6], ...)` is an internal callback typedef (not an ESP-NOW-mandated ABI — safe to change), must stay in sync with Mesh's trampoline/handler above |
| `mesh/MasterBeacon.h` + `.cpp` | `checkTimeout`, `process` | both take `uint8_t lastSeenMasterMac[6]` (out-param) |
| `mesh/RouteMac.h` | `buildHopContext`, `chainStep` | header-only free functions; `uint8_t out_ctx[HOP_CTX_LEN]` uses a named constant, not a literal — still array syntax, still flagged |
| `crypto/Crypto.h` | `reverse32`, `x25519_keygen`, `x25519_shared`, `hmac_sha256`, `aead_seal`, `aead_open` | header-only; 6 functions, various 12/16/32-byte buffers |
| `network/mac_table.h` | `find` | header-only free function, `const uint8_t mac[6]` |
| `network/hw_mac.h` | `cacheDeviceMac`, `readOwnMac` (both `#ifdef UNIT_TEST` branches) | header-only |
| `adapter/Adapter.h` + `.cpp` | `sendDataThroughMesh` | `const uint8_t data[64]` — this is the one pre-existing open alert |

Every change is `T x[N]` → `T* x` (or `const T x[N]` → `const T* x`) in both the declaration
(`.h`) and definition (`.cpp`) where they're split. No parameter is renamed, reordered, or given a
new default. No caller changes.

## Verification

- Full unit + e2e regression must pass identically (pure syntax change, zero behavior delta
  expected — any test failure means something was miscounted as decay-equivalent when it wasn't,
  and needs investigation before proceeding).
- After merge, confirm on the next CodeQL scan that all 7 alerts (6 dismissed + 1 open) resolve to
  `fixed`, not just staying dismissed — this is the actual proof the fix worked, not just that the
  code compiles.

## Global Constraints

Inherited from the umbrella spec: firmware-only, no wire-format changes, no backwards-compat
shims, Tiger Style preserved (no behavior change of any kind, this is pure syntax), full unit + e2e
regression per the plan's task(s), clang-format v18 pinned
(`/opt/homebrew/opt/llvm@18/bin/clang-format`).

## Task Sizing

Small enough for a single task — 9 files, all header-only or header+cpp pairs, all mechanical,
zero call-site churn, zero new tests needed (existing tests already cover these functions'
behavior, which doesn't change). No parallelism needed.
