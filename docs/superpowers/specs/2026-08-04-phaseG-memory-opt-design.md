# Phase G — Memory optimization (#52 + #53)

**Status:** Approved
**Date:** 2026-08-04
**Repos:** lattice-protocol + lattice-nodes + lattice-hub (cross-repo, flag-day protocol v6).
**Scope:** flash-size trims + RAM-residency shrinks + wire-level shrink 250→200B via protocol v6.
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase G).
**Depends on:** Phase 0 (ESP-IDF build + size job), Phase A (NVS rewrite), Phase B (RouteTable pointer + per-origin ReplayCache) — all done.

## Context

Two enhancement issues + wire shrink, bundled per umbrella Phase G:
- **#52 flash:** `docs/memory_usage.md` P1 levers under the current ESP-IDF toolchain — logging `.rodata`, `-Os`/LTO/gc-sections, single AEAD.
- **#53 RAM:** `docs/memory_usage.md` P3 remainder — compact in-RAM residency for `mesh_message` and right-size `recvQueue`/`ROUTE_TABLE_MAX`/`REPLAY_MAX_ORIGINS` bounds.
- **Wire shrink (added per session decision):** protocol v6 flag-day — move secondary-master fields into JOIN_ACK `data[64]` payload (−38B) + reduce `MAX_HOPS` 10 → 8 (`routePath[60→48]`, −12B). Wire drops from 250 → 200B, restoring ESP-NOW headroom.

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

**Constraint:** wire form (post-Phase G, protocol v0.6.0, 200B) is the source of truth. Compact form drops the wire fields that vary by message-type (`enrollment_public_key`, `routePath`, `authTag`, `authPath`, JOIN_ACK-only secondary fields inside `data[64]`) from RAM residency in the recvQueue.

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

### 8. Wire shrink — protocol v6 flag-day (cross-repo)

**Wire target:** 250B → 200B (−50B / 20%). ESP-NOW headroom restored from 0B to 50B.

**Two changes to `MeshMessage`:**

**(a) Move secondary-master fields into JOIN_ACK `data[64]`.** Drop top-level `SecondaryMasterMac[6]` and `SecondaryPublicKey[32]` (proto fields 15+16). In JOIN_ACK frames only, hub packs them into `data[]` at fixed offsets:

```
data[0..4]   = node pubkey fingerprint (existing)
data[4..10]  = secondaryMasterMac (new, JOIN_ACK dual-master only, zero if single-master)
data[10..42] = secondaryPublicKey (new, JOIN_ACK dual-master only, zero if single-master)
data[42..64] = zero
```

Firmware Phase 4+5 `Enrollment::processJoinAck` reads secondary from `data[4..42]` iff `msg.message_type == JOIN_ACK && secondaryMasterMac != all-zero`. Hub `ApproveEnrollment` packs same layout. AEAD `authTag` still covers `data[64]` exactly — secondary bytes are AEAD-protected.

Savings: −38B/frame on every non-JOIN_ACK frame. JOIN_ACK uses previously-wasted `data[]` bytes for what was previously top-level fields.

**(b) Reduce `MAX_HOPS` 10 → 8** in `lattice::config::MAX_HOPS` and shrink `RoutePath[60→48]`. Nanopb `max_size` for `routePath` becomes 48. Deployment-topology cap tightens from 10 to 8 hops; observed deployments never exceeded 4 hops so this is safe with margin.

Savings: −12B/frame.

**Protocol v0.6.0 changes:**
- `message/message.go`: drop `SecondaryMasterMac`, `SecondaryPublicKey`; shrink `RoutePath [60]byte` → `[48]byte`.
- Regen `c/mesh_message.h` (WireSize 200B, static_assert updated).
- Regen `proto/mesh.proto`.
- Tag `v0.6.0`; README versioning table.

**Nodes changes:**
- Submodule pin bump v0.5.0 → v0.6.0.
- `Enrollment.cpp::processJoinAck` reads secondary from `data[4..42]` (post-pin verify, pre-registration).
- `Mesh.cpp` verify `route_len ≤ MAX_HOPS` (now 8, not 10). Existing Phase E clamp already handles.
- `PROTO_VERSION` 4 → 5.

**Hub changes:**
- `go.mod` protocol v0.5.0 → v0.6.0; regen `mesh.pb.go`.
- `ApproveEnrollment` packs secondary bytes into `data[4..42]` when secondary configured (currently sets top-level fields).
- `ProtoVersion` gate 4 → 5 (atomic flag-day, same as Phase C).

**Release-flow order** (per umbrella rule):
1. Protocol PR merges + `v0.6.0` tag.
2. Nodes + hub PRs open in parallel; must merge together (flag-day).

### 9. Audit-driven no-cost wins (items A-Q from post-Phase-G audit)

Reference: `docs/superpowers/specs/2026-08-04-post-phaseG-audit-findings.md`. Adding 17 trivial/low-risk items to Phase G scope. Grouped by touchpoint:

**Delete dead code (item A) — ~1-2 KB flash:**
- `Mesh.cpp` — remove `printMac`, `printMeshMessage`, `generateRandomMeshKey`, `meshKeyIsSet` (all unreferenced).
- `Adapter.cpp` — remove LED stub handler block, `RELAY_ADAPTER` enum value (never referenced), `LED_ADAPTER_DEFAULT_PIN`.
- `Error.h:88-97` — remove `ERROR_ASSERT` + `ERROR_CHECK` templates (no callers).
- `MacAddress.h` — remove `MacAddress(const String&)` sscanf ctor (no callers; pulls libc format tables).

**Right-size RAM state:**
- Item B: `LATTICE_E2E_KEYCACHE_MAX` role-split — leaves get 2 (primary+secondary master); masters get 10. Mirror `reevaluateRouteTable` role-conditional pattern. **~576 B RAM per leaf.**
- Item C: `Enrollment::PENDING_RELAY_QUEUE_SIZE` 8→4 to track `RECV_QUEUE_SIZE`. **~152 B RAM.**
- Item D: `ReplayCache::Entry` field reorder for padding: `{uint32 epoch, uint32 lastSeenMs, uint16 seq, uint8 mac[6], bool used}`. **48 B RAM.**
- Item K: `pir/PirAdapter.h::_cooldownSeconds` → `constexpr`; collapse `_timerActive`/`_motionSent`. ~6-10 B/PIR.
- Item L: pin types `int` → `uint8_t` across `Adapter.h` + factory. ~3-4 B/adapter.

**DRY helpers + consolidation:**
- Item E: consolidate FF:FF broadcast MAC — 7 `static const uint8_t[6]` sites into one `constexpr` header. ~50 B flash.
- Item N: 3 copies of `readOwnMac(uint8_t[6])` (PirAdapter, SerialAdapter, SerialFraming) → one helper in `hw_mac.h`. ~150 B flash.
- Item Q: canonical `bool lattice::mac::eq(const uint8_t*, const uint8_t*)` — replaces ~60 sites of two competing idioms (`memcmp(a,b,6)==0` and `MacAddress(a) == MacAddress(b)`; the latter is worse — extra `memcpy`). ~1.5-2 KB flash.
- Item J: `GpioInput::isValidPin` / `GpioOutput::isValidPin` `switch` → `constexpr uint64_t VALID_MASK; return pin<64 && (MASK>>pin)&1;`. ~150-300 B flash.
- Item M: `SevenSegDisplay::show()` and `showWithDP()` → one `showInternal(value, leadingZeros, bool withDP)`. 200-350 B flash.

**Compile-time / type wins:**
- Item H: `std::function` → plain function-pointer typedefs. Sites: `Mesh.h::externalRecvCallback`, `Enrollment.h::RegisterPeerFn` + `EnrollmentRelayFn`. All current bindings are function pointers or captureless lambdas. **~100 B RAM + ~1 KB flash.**
- Item I: `GpioInput`/`GpioOutput` drop `virtual` on `init()` — never dispatched polymorphically (grep confirms no `GpioInput*` dispatch). Keeps Adapter virtual — that IS dispatched via `Adapter*`. ~200-400 B flash + pointer/instance.

**Correctness / robustness:**
- Item F: cache `esp_wifi_get_mac(WIFI_IF_STA, ...)` result at boot — currently called on every RX-frame (4-6 sites in Adapter, SerialAdapter, SerialFraming). MAC doesn't change. CPU win + minor flash.
- Item G: cache `Enrollment::isEnrolled()` — currently reads NVS via `_prefs.getBool` every call, called 2-3× per `loop()` from `main.cpp`. Set cached bool in `init()`/`processJoinAck()`/`saveEnrolledFlag()`. Eliminates per-loop NVS I/O.
- Item O: extend `_persistOrEscalate` (Phase A) to all 18 NVS put-sites (currently used at only 5). Non-security sites get `securityRelevant=false` so short-write failures don't silently discard. ~150 B flash + robustness.
- Item P: extend `MbedtlsGuard.h` (Phase E) with `EcpGroupCtx`, `MpiCtx`, `EcpPointCtx`, `ChaChaPolyCtx` — Phase E left these hand-managed in `MeshCrypto.h::generateKeypair` and `E2ECrypto.h::sealPayload/openPayload`. Same UNIT_TEST-unwind leak class the guard fixed elsewhere.

**Cumulative est. impact (items A-Q):** ~1.5 KB RAM + ~5-8 KB flash on top of the wire-shrink + CompactMessage + bounds-tune savings already in §1-§8.

### 10. CI size measurement

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

- No new features.
- Not deep-tuning mbedtls beyond disabling AES/GCM/CCM.
- Not refactoring `Logger` runtime dispatch — only add compile-time macros.
- Not truncating `authTag` (security downgrade; ChaCha20-Poly1305 128-bit stays).
- Not dropping `enrollment_public_key` — Phase D pin check needs it on the wire.
- Not doing nanopb variable-length wire encoding (too broad; deferred).

## Testing

**Unit:**
- `test_compact_message.cpp` (new): round-trip `wire → compact → wire` preserves every field the compact form stores; skipped fields are absent (`0`).
- Existing `test_mesh_logic.cpp` + `test_route_report.cpp` + `test_e2e_crypto.cpp` regression — must still pass with the queue type change.

**E2E:**
- Existing routing + enrollment + aead scenarios must remain green. If bounds cuts (RECV_QUEUE_SIZE 4) cause queue-full drops in an existing test, either raise the queue slightly or adjust the test's send rate.

**Size:**
- Report `idf.py size` before/after in PR body. No hard threshold — informational.

## Files touched (estimate)

**lattice-protocol (v0.6.0 tag):**
- `message/message.go` — drop 2 fields, shrink `RoutePath` array (~5 LOC).
- Regen `c/mesh_message.h`, `proto/mesh.proto`, `proto/mesh.options` (nanopb `max_size`).
- README versioning table.

**lattice-nodes:**
- `firmware/main/lib/lattice-protocol` submodule pin bump.
- `firmware/main/project_config.h` — `RECV_QUEUE_SIZE`, `LATTICE_ROUTE_TABLE_MAX`, `LATTICE_REPLAY_MAX_ORIGINS`, `MAX_HOPS`, `LATTICE_DEFAULT_LOG_LEVEL` (~15 LOC).
- `firmware/sdkconfig.defaults` — `-Os`/LTO/mbedtls trim (~10 LOC).
- `firmware/main/src/logging/Logger.h` — new macros (~20 LOC).
- All log-call sites — mechanical `Logger::log*` → `LATTICE_LOG*` (grep + rewrite; ~100 sites).
- `firmware/main/src/mesh/Enrollment.cpp` — read secondary from `data[4..42]` in JOIN_ACK path.
- `firmware/main/src/mesh/Mesh.h` — `recvQueue` type change; `PROTO_VERSION` 4 → 5.
- `firmware/main/src/mesh/Mesh.cpp` — decode/encode boundary edits.
- `firmware/main/src/mesh/CompactMessage.{h,cpp}` — new (~120 LOC).
- `tests/unit/test_compact_message.cpp` — new (~80 LOC).
- `tests/unit/test_enrollment.cpp` (or `test_mesh_logic.cpp`) — dual-master JOIN_ACK read from `data[]` cases.

**lattice-hub:**
- `server/orchestrator/go.mod` — protocol v0.6.0.
- `server/orchestrator/mesh/mesh.pb.go` — regen.
- `server/orchestrator/mesh/server.go::ApproveEnrollment` — pack secondary into `data[4..42]` when configured; drop top-level secondary field writes.
- `server/orchestrator/mesh/server.go` — `ProtoVersion` gate 4 → 5 atomically.
- Test fixtures updated for the layout change.

Rough size: nodes ~400 LOC production + 200 LOC test; hub ~50 LOC; protocol ~5 LOC + regen.
