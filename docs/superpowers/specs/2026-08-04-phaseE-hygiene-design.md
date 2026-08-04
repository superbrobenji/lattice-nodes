# Phase E — Hygiene sweep (#47)

**Status:** Approved
**Date:** 2026-08-04
**Repo:** primary `lattice-nodes`; small tail in `lattice-protocol`.
**Scope:** low-severity code-hygiene items surfaced across Phase 1–5 reviews. No wire-format changes. No functional behaviour change in shipped configuration.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase E).

## Context

Six items, all Low, all independently addressable. None affect shipped correctness or security. Bundling as one Phase E to close #47 in a single PR rather than spread across weeks.

## Design

### 1. mbedtls context leak on the `err::fatal` path

**Where:** `firmware/main/src/mesh/E2ECrypto.h::computeSharedSecret` (fatal digits 20-25/27), `E2ECrypto.h::deriveE2EKeys` (fatal digit 26), and the same pattern in `firmware/main/src/mesh/MeshCrypto.h::generateKeypair`.

**Bug:** each function initialises mbedtls contexts (`mbedtls_ecdh_init`, `mbedtls_entropy_init`, `mbedtls_ctr_drbg_init`), and on failure calls `lattice::err::fail(...)` before the paired `_free` calls. `err::fail` is `[[noreturn]]` on-device (reboots), so in production the leak is moot. Under `UNIT_TEST` it throws a `FatalError`, unwinds the stack, and the contexts leak — polluting subsequent tests and eventually exhausting the mock heap.

**Fix:** RAII wrapper — a small stack-scope guard struct per context that calls the matching `_free` in its destructor. Applied once, then all fatal paths auto-clean. Choose the guard form over hand-inserting `_free()` before every `fail()` (5+ sites in `computeSharedSecret` alone) to avoid an incomplete cleanup on some future added fatal.

```cpp
// firmware/main/src/mesh/MbedtlsGuard.h (new header-only)
namespace lattice::mesh::mbedtls_guard {
struct EcdhCtx  { mbedtls_ecdh_context ctx{};      ~EcdhCtx()  { mbedtls_ecdh_free(&ctx); }       operator mbedtls_ecdh_context*()      { return &ctx; } };
struct EntropyCtx { mbedtls_entropy_context ctx{}; ~EntropyCtx() { mbedtls_entropy_free(&ctx); }  operator mbedtls_entropy_context*()   { return &ctx; } };
struct CtrDrbgCtx { mbedtls_ctr_drbg_context ctx{}; ~CtrDrbgCtx() { mbedtls_ctr_drbg_free(&ctx); }  operator mbedtls_ctr_drbg_context*()  { return &ctx; } };
}
```

Each guard calls the mbedtls `_init` inline via the default constructor invoking the mbedtls init function; verify at implementation time that C-struct default-init suffices (may need explicit `_init` call in the guard ctor).

Every call site in `E2ECrypto.h` and `MeshCrypto.h` replaces the raw `mbedtls_*_context` locals with a guard, then drops the trailing `_free` calls (redundant with the guard's dtor). Fatal paths just `return` after `err::fail` (unreachable in prod, guard-cleaned in tests).

### 2. Implementation-defined signed right-shift

**Where:** `firmware/main/src/mesh/E2ECrypto.h::buildAad` (lines 120-122) and `buildNonce` (already OK per Phase A read but verify) — `msg.data_type >> 8/16/24` on `int32_t`.

**Fix:** cast to `uint32_t` first.

```cpp
uint32_t dt = static_cast<uint32_t>(msg.data_type);
aad[N+0] = static_cast<uint8_t>(dt);
aad[N+1] = static_cast<uint8_t>(dt >> 8);
aad[N+2] = static_cast<uint8_t>(dt >> 16);
aad[N+3] = static_cast<uint8_t>(dt >> 24);
```

Verify `buildNonce` uses the same pattern; if so, apply the same cast.

### 3. Serial relay `proto_version` literal

**Where:** `firmware/main/src/adapter/serial/SerialAdapter.cpp:158` (`relayEnrollmentToServer`) — `msg.proto_version = 1;`.

**Fix:** replace with `msg.proto_version = lattice::mesh::PROTO_VERSION;` (currently `4` post-Phase C).

Rationale confirmed by user: hub gates inbound at `== 4` post-flag-day, and every other outbound stamp on nodes already uses the constant. Literal `1` here would be an inconsistency after Phase C; the umbrella note about "harmless because it's serial-layer" applied under v3, not v4.

Note: this exact line was noted as a "pre-existing bug flagged for separate ticket" during Phase C Task 2 fix round 1. Phase E closes it now.

### 4. Inline `route_len <= MAX_HOPS` clamp in `sendDownlinkToNode`

**Where:** `firmware/main/src/mesh/Mesh.cpp::sendDownlinkToNode` — the branch that copies `path`/`pathLen` from `routes->lookup(...)` into `msg.route_path` / `msg.route_len`.

**Fix:** defensive clamp before indexing:

```cpp
if (pathLen > lattice::config::MAX_HOPS) {
  Logger::logln("MESH", "downlink route_len exceeds MAX_HOPS — dropping", LogLevel::LOG_ERROR);
  return;   // or truncate; err on side of drop
}
```

Currently transitively safe (`RouteTable::record` clamps at write-time), but a defensive inline check makes the bound local and refactor-proof.

### 5. Downlink LRU enrolled/master guard — runtime check (per user)

**Where:** `firmware/main/src/mesh/Mesh.cpp::registerDownlinkPeer` — the LRU-touch branch (`for (size_t i = 0; i < downlinkPeerLruCount; ++i) { ... memcpy touched ... }`).

**Fix:** at the top of the LRU-touch branch, re-check enrolled/master membership before touching the LRU. Currently the enrolled/master short-circuit runs before the LRU touch on first entry, but if a MAC's status changes AFTER it lands in the LRU, subsequent registerDownlinkPeer calls would still touch it. Belt-and-braces:

```cpp
for (size_t i = 0; i < downlinkPeerLruCount; ++i) {
  if (lattice::utils::MacAddress(downlinkPeerLru[i]) == lattice::utils::MacAddress(mac)) {
    // Defensive re-check: if this MAC has since become enrolled or the master,
    // remove it from the LRU (its peering is now managed by its own path).
    if (peers.find(mac) || (currentMaster.distance != 0xFF &&
        lattice::utils::MacAddress(mac) == lattice::utils::MacAddress(currentMaster.mac))) {
      for (size_t j = i; j + 1 < downlinkPeerLruCount; ++j) {
        memcpy(downlinkPeerLru[j], downlinkPeerLru[j + 1], 6);
      }
      downlinkPeerLruCount--;
      lattice::mesh::crypto::registerPeerWithEspNow(mac);
      return;
    }
    // ... existing touch logic (move to front) ...
  }
}
```

Small extra work per LRU hit but defense-in-depth: LRU never grows unbounded via a MAC that has "graduated" to enrolled/master status.

### 6. `lattice-protocol` `gofmt`

**Where:** `message/message.go` — proto-v3 field lines have a stray double-space before the type name (`gofmt -l` flags it).

**Fix:** run `gofmt -w message/message.go` from the protocol repo root. Verify `make check` passes after (codegen output byte-identical). No wire change.

Small standalone protocol PR; no version bump.

## Non-goals

- No wire-format changes.
- No new features or logging beyond the item-5 clamp log.
- No refactoring beyond what the RAII guard requires.
- Not touching Phase F (hub #63/#64) or Phase G (memory opt).

## Testing

**Nodes:**
- Existing E2E crypto unit tests (`tests/unit/test_e2e_crypto.cpp`) exercise `computeSharedSecret`/`deriveE2EKeys` happy paths — must still pass with the RAII guard swap. Any pre-existing fatal-path test needs to verify the guards ran (no direct heap-leak assertion available; rely on process not blowing up across many failing test cases).
- Item 4 clamp: extend `tests/unit/test_route_report.cpp` or `test_mesh_logic.cpp` with a case where `RouteTable::record` is bypassed (via `testRoutes()->` direct write with an oversized len) and `sendDownlinkToNode` is called → assert drop + no crash. If the direct-write path is too invasive, skip the new test and rely on code inspection.
- Item 5 guard: extend `test_mesh_logic.cpp` with `RegisterDownlinkPeer_LRUEntryBecomesEnrolled_EvictsOnNextTouch` — pre-populate LRU with a MAC, then add that MAC to `peers`, then re-call `registerDownlinkPeer(mac)`; assert LRU no longer contains it.
- Full unit suite green (`--label-exclude e2e`) + e2e route+enroll green.

**Protocol:**
- `make check` clean after `gofmt -w`.
- `go test ./...` green.

## Files touched (estimate)

**lattice-nodes:**
- `firmware/main/src/mesh/MbedtlsGuard.h` (new, ~30 LOC)
- `firmware/main/src/mesh/E2ECrypto.h` — swap contexts to guards; cast `data_type`; ~30 LOC delta
- `firmware/main/src/mesh/MeshCrypto.h` — same guard swap; ~10 LOC delta
- `firmware/main/src/adapter/serial/SerialAdapter.cpp` — `1` → `PROTO_VERSION`; 1-line change
- `firmware/main/src/mesh/Mesh.cpp` — clamp in `sendDownlinkToNode`; runtime re-check in `registerDownlinkPeer`; ~20 LOC delta
- `tests/unit/test_route_report.cpp` OR `test_mesh_logic.cpp` — optional item-4 test
- `tests/unit/test_mesh_logic.cpp` — item-5 test

**lattice-protocol:**
- `message/message.go` — `gofmt -w` (whitespace only)

Rough size: ~90 LOC production + ~50 LOC tests in nodes; 0 net LOC in protocol.
