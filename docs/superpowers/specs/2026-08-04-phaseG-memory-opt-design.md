# Phase G — Memory optimization (#52 + #53)

**Status:** Approved
**Date:** 2026-08-04
**Repo:** lattice-nodes (only).
**Scope:** flash-size trims + RAM-residency shrinks. Measurement-gated on the ESP-IDF `firmware-build.yml` size job (Phase 0). No wire-format changes (compact form is nodes-side RAM only). No cross-repo work.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase G).
**Depends on:** Phase 0 (ESP-IDF build + size job), Phase A (NVS rewrite), Phase B (RouteTable pointer + per-origin ReplayCache) — all done.

## Context

Two enhancement issues, bundled per umbrella Phase G:
- **#52 flash:** `docs/memory_usage.md` P1 levers under the current ESP-IDF toolchain — logging `.rodata`, `-Os`/LTO/gc-sections, single AEAD.
- **#53 RAM:** `docs/memory_usage.md` P3 remainder — compact in-RAM residency for `mesh_message` and right-size `recvQueue`/`ROUTE_TABLE_MAX`/`REPLAY_MAX_ORIGINS` bounds.

Success measured via CI's `firmware-build.yml` `idf.py size` job — deltas reported in the PR body, no local ESP32 required.

## Design

### 1. Flash: compile-time log gating (`#52` lever 1)

**Where:** `firmware/main/src/logging/Logger.h`, all `Logger::log*` call sites.

**Fix:** replace `Logger::log*(...)` calls with macros that expand to `((void)0)` when `DEFAULT_LOG_LEVEL == LOG_NONE`. Format strings then never reach the object file's `.rodata`.

```cpp
// Logger.h — new macros; existing Logger::log/logln remain for callers that
// need runtime routing.
#if LATTICE_DEFAULT_LOG_LEVEL <= LATTICE_LOG_LEVEL_NONE
  #define LATTICE_LOG(tag, msg, level)   ((void)0)
  #define LATTICE_LOGLN(tag, msg, level) ((void)0)
#else
  #define LATTICE_LOG(tag, msg, level)   ::lattice::utils::Logger::log(tag, msg, level)
  #define LATTICE_LOGLN(tag, msg, level) ::lattice::utils::Logger::logln(tag, msg, level)
#endif
```

`LATTICE_DEFAULT_LOG_LEVEL` becomes a compile-time integer (mirrors runtime enum) defined in `project_config.h`. Existing `constexpr` `DEFAULT_LOG_LEVEL` stays for runtime behaviour; the macro path is additive.

**Migration:** `Logger::logln("TAG", ...)` → `LATTICE_LOGLN("TAG", ..., LogLevel::LOG_INFO)` mechanically. Ripgrep pass across `firmware/main/src/`.

**Est. saving:** 5-15 KB `.rodata` when log-level compiled to NONE.

### 2. Flash: verify build flags (`#52` lever 2)

**Where:** `firmware/sdkconfig.defaults`.

**Fix:** verify these are set; if not, add:

```
CONFIG_COMPILER_OPTIMIZATION_SIZE=y   # -Os
CONFIG_COMPILER_OPTIMIZATION_LTO=y    # link-time
# gc-sections + function-sections are ESP-IDF defaults (verify)
```

If ESP-IDF already sets these post-Phase-0, this is a no-op verification.

**Est. saving:** 5-15 KB depending on prior state.

### 3. Flash: mbedtls trim to single AEAD (`#52` lever 3)

**Where:** `firmware/sdkconfig.defaults`.

**Fix:** disable AES + GCM modules (they're not used — E2E is ChaCha20-Poly1305 only per Phase A):

```
CONFIG_MBEDTLS_AES_C=n
CONFIG_MBEDTLS_GCM_C=n
CONFIG_MBEDTLS_CCM_C=n
```

**Risk:** other mbedtls features may transitively depend on AES (e.g. TLS RSA sessions). Verify by attempting build with these off and checking link. If a dependency is real (e.g. `mbedtls_pk_verify` requires AES-CBC internally), roll back the specific config and keep the others. Document what stayed enabled and why.

**Est. saving:** 5-15 KB.

### 4. RAM: RECV_QUEUE_SIZE 8 → 4 (`#53` lever 2a)

**Where:** `firmware/main/project_config.h` — `RECV_QUEUE_SIZE`.

**Fix:** `constexpr size_t RECV_QUEUE_SIZE = 4;`.

`recvQueue[RECV_QUEUE_SIZE] * sizeof(mesh_message)` = 4 × 250B = 1 KB (vs 2 KB at 8). Post-compact-form (lever 6), the per-slot cost drops further.

**Rationale:** observed fan-in during Phase A + B testing never exceeded 3 concurrent frames. 4-deep gives a 1-slot margin. If real deployments show queue-full drops, raise back.

**Est. saving:** 1 KB (or more with compact form).

### 5. RAM: LATTICE_ROUTE_TABLE_MAX 32 → 16 (`#53` lever 2b, master-only)

**Where:** `firmware/main/project_config.h` — `LATTICE_ROUTE_TABLE_MAX`.

**Fix:** `constexpr size_t LATTICE_ROUTE_TABLE_MAX = 16;`.

Post-Phase-B, `RouteTable` is heap-allocated only on masters. 16 entries × 72B = 1.15 KB per master (vs 2.3 KB). Sufficient for realistic deployment fan-out; raise if a master ever accumulates more than 16 distinct nodes.

**Est. saving:** 1.15 KB on masters. Zero on leaves.

### 6. RAM: LATTICE_REPLAY_MAX_ORIGINS 16 → 12 (`#53` lever 2c)

**Where:** `firmware/main/project_config.h` — `LATTICE_REPLAY_MAX_ORIGINS`.

**Fix:** `constexpr size_t LATTICE_REPLAY_MAX_ORIGINS = 12;`.

Per-origin high-water (Phase B). 12 × sizeof(Entry) ≈ 12 × 21B = ~250 B (vs ~340 B).

**Est. saving:** ~90 B. Marginal but included because of umbrella P3 coverage.

### 7. RAM: compact `mesh_message` in-RAM residency (`#53` lever 1)

**Where:** `firmware/main/src/mesh/CompactMessage.h` (new), `Mesh.h::recvQueue`, `Mesh::messageProcessor` decode/encode boundary.

**Constraint:** wire form (`mesh_message`, protocol v0.5.0, 250B) stays exactly as-is. Compact form is a NODES-side RAM optimisation only. Every field in the wire struct is used somewhere; a wire shrink would need protocol v6 (flag-day) for uncertain win and require sub-ESP-NOW-cap coordination across all consumers. Scope-defer that.

**Compact form:**

```cpp
// firmware/main/src/mesh/CompactMessage.h
namespace lattice::mesh {

// Compact in-RAM representation of mesh_message for buffered/queued processing.
// Wire form (250B) is preserved via the SerialFraming/ESP-NOW send path;
// this is only the RAM residency of frames sitting in the recvQueue or
// carried on the stack past initial decode.
struct CompactMessage {
  uint8_t  proto_version;
  uint8_t  message_type;
  int32_t  data_type;
  uint8_t  origin_mac[6];
  uint8_t  target_mac[6];
  uint8_t  last_hop_mac[6];
  uint8_t  data[64];
  uint8_t  hop_count;
  uint32_t epoch_num;
  uint16_t seq_num;
  uint16_t auth_tag_offset;   // if authTag present, points into data[] region on wire path
  // Skipped in RAM: public_key[32], routePath[60], routeLen,
  //                 secondary_master_mac[6], secondary_public_key[32],
  //                 auth_tag[16] (full copy), auth_path[8]
  // These are extracted on-demand from the wire buffer at the decode boundary
  // only when the specific message type needs them (JOIN_ACK, RouteReport).
};
static_assert(sizeof(CompactMessage) <= 128, "CompactMessage residency budget");

// Convert wire → compact for enqueue.
void toCompact(const mesh_message& src, CompactMessage& dst);
// Convert compact → wire for send. If sending a JOIN_ACK or RouteReport,
// the caller must re-populate the type-specific fields from source-of-truth
// state (they're not stored in CompactMessage).
void toWire(const CompactMessage& src, mesh_message& dst);
}
```

`sizeof(CompactMessage)` ≈ 100B (vs 250B). `recvQueue[4] × 100B` = 400B (vs `recvQueue[4] × 250B` = 1000B). Saves 600B in queue + wins on stack copies.

**Migration:** everything past `messageProcessor` decode consumes `CompactMessage`. Type-specific handlers (JOIN_ACK receive, RouteReport verify) look up their extra fields from the raw wire buffer OR from persistent state (JOIN_ACK verification uses pinned MASTER_PUBKEY, not `msg.enrollment_public_key` — Phase D). RouteReport verify runs BEFORE compact conversion since `auth_path` + `route_path` are wire-form only.

**No wire coordination needed:** nothing on the wire moves. The nanopb `static_assert(sizeof(mesh_message) == 250, ...)` guard in the generated `c/mesh_message.h` still fires; compact form doesn't intersect it.

**Est. saving:** ~600B queue + reduced stack pressure. Worth the refactor if measurements confirm.

### 8. CI size measurement

Use existing `firmware-build.yml` `idf.py size` job. Post the size delta in the PR body:

```
Before (main @ SHA):
  DRAM     8912 bytes total, 4560 free
  IRAM   135218 bytes total, ...
  Flash  789456 bytes total, 34210 free
After (feat/phaseG):
  DRAM     8912 bytes total, 6320 free   (+1760 B free)
  Flash  749812 bytes total, 73854 free  (+39644 B free)
```

Numbers illustrative; PR body carries actuals.

## Non-goals

- No wire-format changes. `mesh_message` stays exactly 250 B.
- No protocol repo touches.
- No hub touches.
- No new features.
- Not deep-tuning mbedtls beyond disabling AES/GCM/CCM.
- Not refactoring `Logger` runtime dispatch — only add compile-time macros.
- Not touching MAX_HOPS (Phase B held it at 10; changing it would rebalance route_path wire cost).

## Testing

**Unit:**
- `test_compact_message.cpp` (new): round-trip `wire → compact → wire` preserves every field the compact form stores; skipped fields are absent (`0`).
- Existing `test_mesh_logic.cpp` + `test_route_report.cpp` + `test_e2e_crypto.cpp` regression — must still pass with the queue type change.

**E2E:**
- Existing routing + enrollment + aead scenarios must remain green. If bounds cuts (RECV_QUEUE_SIZE 4) cause queue-full drops in an existing test, either raise the queue slightly or adjust the test's send rate.

**Size:**
- Report `idf.py size` before/after in PR body. No hard threshold — informational.

## Files touched (estimate)

- `firmware/main/project_config.h` — 3 bound tunings + `LATTICE_DEFAULT_LOG_LEVEL` compile-time constant (~10 LOC).
- `firmware/sdkconfig.defaults` — `-Os`/LTO/mbedtls trim knobs (~10 LOC).
- `firmware/main/src/logging/Logger.h` — new macros (~20 LOC).
- All log-call sites — mechanical `Logger::log*` → `LATTICE_LOG*` (grep + rewrite; ~100 sites).
- `firmware/main/src/mesh/CompactMessage.{h,cpp}` — new (~120 LOC).
- `firmware/main/src/mesh/Mesh.h` — `recvQueue` type change.
- `firmware/main/src/mesh/Mesh.cpp` — decode/encode boundary edits.
- `tests/unit/test_compact_message.cpp` — new (~80 LOC).
- Existing tests may need `CompactMessage`-aware fixture updates.

Rough size: ~300 LOC production + tooling; ~80 LOC test.
