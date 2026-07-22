# Umbrella Design: Close All Open Issues (lattice-nodes + lattice-hub + lattice-protocol)

**Date:** 2026-07-22
**Status:** Approved (umbrella) — per-phase implementation plans generated on pickup
**Scope:** 20 open issues across three repos, decomposed into 11 sequenced phases.

> **Revision (2026-07-22, during Phase 0 planning):** Driving the real toolchain revealed that after #49's three compile blockers are fixed, the firmware **fails to link** — `MBEDTLS_CHACHAPOLY_C` is disabled in the ESP32 Arduino core, so the Phase 1 E2E AEAD (ChaCha20-Poly1305) has no implementation (filed as **#54**). The shipped E2E crypto has never linked on this toolchain. Fix: **migrate the build to ESP-IDF (arduino-as-component)** with a custom `mbedtls_config`. This promotes the formerly-deferred ESP-IDF migration into the **foundational Phase 0**, and folds the previously-deferred flash levers (drop-BT, mbedtls-trim) into scope.

## Purpose

Close every open issue across the Lattice mesh system. This document is the **decomposition and sequencing spec** — it maps every issue to a phase, records the locked design decision for each, and defines the cross-repo release order. It does **not** contain task-level implementation detail; each phase gets its own implementation plan (via the writing-plans skill) when it is picked up.

## Global constraint: no backwards compatibility

The system supports the **latest protocol only**. Devices are reflashed; there is no fielded fleet to keep compatible. This is a hard, project-wide simplification:

- No data migration paths — persisted state may be reset on reflash.
- Protocol/wire changes are flag-day. The hub already drops any frame with `ProtoVersion != 3` (`server.go:364`); the next bump is a clean cutover.
- No version negotiation between nodes, master, or hub.

## Issue → phase map

| Phase | Issues | Repo(s) | One-line |
|---|---|---|---|
| 0 — ESP-IDF migration | #49, #54 | nodes | Migrate build to ESP-IDF (arduino-as-component) — links chachapoly, enables mbedtls/BT config, fixes the 4 build blockers + CI build/size job |
| H — Hub enrollment | #87, #85, #86 | hub | Master keypair → correct JOIN_ACK → fix test. P0: enrollment is broken today |
| H2 — Hub dual-master | #88 | hub | Populate secondary-master fields in JOIN_ACK (hub half of shipped firmware Phase 4/5) |
| A — Persistence | #50, #43 | nodes | Migrate EepromManager → NVS/Preferences (clean start); fix DEV_MODE epoch + commit checks |
| B — Routing | #45, #46, #51 | nodes | Distance-freshness coupling; replay high-water; RouteTable role-gating |
| C — Downlink auth | #44 | protocol → nodes + hub | Per-hop HMAC-authenticated route_path; flag-day v4 proto field |
| D — Enrollment harden | #42 | nodes | Pin master pubkey at flash; node verifies JOIN_ACK key against it |
| E — Hygiene | #47 | nodes + protocol | 6 code-hygiene items + lattice-protocol `gofmt` |
| F — Hub misc | #63, #64 | hub | Empty-name enrollment default; meshsim write-under-mutex deadlock |
| G — Optimization | #52, #53 | nodes | Flash trim (incl. drop-BT + mbedtls-trim, now unlocked by Phase 0's IDF build) + RAM residency/bounds (memory_usage.md P1/P3) |
| (S) — folded into H | new | hub | Set `ProtoVersion=3` on all outbound frames + CI `mesh.pb.go`↔proto sync check |

## Dependency graph & sequencing

```
Phase 0 (nodes)  ──┐ gates all node-firmware verification + memory re-measure
                   │
Phase H (hub)  ────┼── gates Phase D (enrollment must work before it can be hardened)
                   │    gates Phase H2
Phase H2 (hub)     │
                   │
Phase A (nodes) ───┤ (needs Phase 0 green build)
Phase B (nodes) ───┤ (needs Phase 0; internally parallelizable)
Phase C (3 repos)──┤ (needs Phase 0 build + Phase H's proto-sync CI)
Phase D (nodes) ───┘ (needs Phase 0 build + Phase H working enrollment)
Phase E (nodes+proto)  rides Phase C's protocol release
Phase F (hub)          fully independent — any time
Phase G (nodes)        measurement-gated: needs Phase 0 (size job) + banks A (EEPROM) & B (RouteTable) first
```

- **Phase 0 and Phase H are in different repos with no shared dependency** — plan/execute concurrently.
- **Phase B and Phase F are independent** — parallelizable.
- **Phase C is the only multi-repo release.** Order: lattice-protocol change → release + tag → lattice-nodes submodule re-point + firmware → lattice-hub `go.mod` bump + `mesh.pb.go` regen. Never merge nodes against a floating protocol branch SHA (release-flow rule).

## Per-issue approach (locked decisions)

### Phase 0 — #49 + #54 (ESP-IDF migration, nodes)
**Goal:** a firmware image that **compiles AND links** with a reproducible non-IDE toolchain and a CI size report. Driving the real toolchain (arduino-cli 1.5.1 + esp32@3.3.10) proved the current build is broken at two levels — three compile blockers, then a link failure the compile errors were masking.

**The four verified blockers:**
1. **nanopb include path** — `mesh.pb.h:6` does `#include <pb.h>`; `main/src/mesh/serialization/nanopb` is not on the include path. *Verified fix:* add it as an include dir. (Under ESP-IDF: a CMake `target_include_directories`.)
2. **Non-self-contained headers** — `ButtonHandler.h` uses unqualified `Logger`/`LogLevel` (9 sites; verified it is the *only* affected file). *Verified fix:* fully-qualify `lattice::utils::Logger`/`LogLevel`. Correctness fix regardless of toolchain; overlaps #47.
3. **Duplicate `mesh_message` typedef** — **not** `lib/` auto-scan as originally filed (arduino-cli adds no such `-I`, verified via `compile_commands.json`). Real cause: `mesh_message.h`'s `#pragma once` fails to dedupe across two include-path spellings. *Verified fix:* a traditional `#ifndef` guard — emitted **upstream in `cmd/gen-headers`** (lattice-protocol) so it survives regeneration.
4. **Link failure — `mbedtls_chachapoly_*` undefined (#54)** — `MBEDTLS_CHACHAPOLY_C` is disabled in the ESP32 Arduino core's mbedtls, so the Phase 1 E2E AEAD (ChaCha20-Poly1305) has no implementation. Verified: chachapoly symbols absent from the core's `libmbedcrypto.a`; gcm/sha256 present. **This is why the migration is pulled forward** — a custom `mbedtls_config` under ESP-IDF is the clean, cipher-and-wire-preserving fix.

**Approach — ESP-IDF with arduino-as-component.** Migrate off bare Arduino/`arduino-cli` to an ESP-IDF (CMake) project that pulls in `arduino-esp32` as a component, so the existing Arduino API surface (`Arduino.h`, `EEPROM`, `esp_now`, `String`, etc.) keeps working with minimal source change, while gaining a custom `sdkconfig` + `mbedtls_config` we control. This: (a) enables `MBEDTLS_CHACHAPOLY_C` → resolves #54 with cipher + wire unchanged; (b) unlocks the deferred flash levers now folded into Phase G (`CONFIG_BT_ENABLED=n`, mbedtls suite trim); (c) gives a reproducible CI build + `xtensa-esp32-elf-size` size report that regenerates `docs/memory_usage.md`.

**CI:** replace the (nonexistent) firmware build with an ESP-IDF build job + size-report artifact. **Gate:** a green build+link required before any later firmware phase (A/B/C/D/E/G). The exact migration steps — CMake project layout, `idf_component.yml`, `sdkconfig` defaults, source relocation — are detailed in the Phase 0 implementation plan.

### Phase H — #87 → #85 → #86 (hub enrollment, P0)
Enrollment is broken on `main`: hub JOIN_ACK is malformed and every node rejects it (`Enrollment.cpp:95` fingerprint check fails). Ordered fix:
- **#87 (prerequisite):** hub has no master Curve25519 keypair. Generate on first start (`crypto/rand` + `curve25519`), persist to `masterkey.json` (0600, `MASTER_KEY_PATH` env, default `/data/masterkey.json`), load on subsequent starts. Add `MasterPublicKey`/`MasterPrivateKey` to `MeshServerConfig`. Hub must also learn the master's serial-adapter MAC (env or read at startup) for `OriginMacAddress`.
- **#85:** rebuild JOIN_ACK in `ApproveEnrollment` (`server.go:678`): `PublicKey` = master pubkey; `OriginMacAddress` = master serial MAC; `Data[0:4]` = first 4 bytes of the enrolling node's pubkey; **`ProtoVersion = 3`**.
- **#86:** update `server_enrollment_test.go` to assert master pubkey + correct MAC + fingerprint (it currently asserts the node's key, hiding #85).
- **Folded (Phase S):** set `ProtoVersion=3` on **all** outbound frames (JOIN_ACK, OP_NODE_ID_SET, CONFIG_SET, health-request, TX-power — only `SendNodeData` sets it today). Add a CI step asserting `mesh.pb.go` is regen-clean against `mesh.proto` + the `go.mod` protocol pin (no codegen check exists today; this is how ProtoVersion drift went unnoticed).

### Phase H2 — #88 (hub dual-master, hub)
Firmware Phase 4/5 fully implements node-side secondary master; hub never populates the wire fields. Add `SecondaryMasterPublicKey`/`SecondaryMasterMAC` to `MeshServerConfig`; when `DUAL_MASTER_ENABLED` and a secondary port is configured, `ApproveEnrollment` sets proto field 15 (`secondaryMasterMac`) + field 16 (`secondaryPublicKey`) in JOIN_ACK. Secondary master needs its own keypair (reuses #87 infrastructure). Secondary MAC via `SECONDARY_MASTER_MAC` env (explicit; simplest). Depends on Phase H.

### Phase A — #50 + #43 (persistence, nodes)
- **#50:** rewrite `EepromManager` onto NVS `Preferences` (key/value), dropping the fixed 512-byte map, manual address arithmetic, and `SCHEMA_VERSION` gating. **No migration path** (no-backcompat: clean NVS start on reflash). Removes the 98%-full ceiling permanently.
- **#43 (absorbed into the rewrite):** in DEV_MODE keep a RAM-only monotonically-increasing epoch seed (refuse to E2E-seal if the epoch cannot advance) so dev builds never reuse AEAD nonces across reboots; check every NVS-write/`commit()` return and escalate/log on failure (a failed epoch persist is security-relevant).

### Phase B — #45, #46, #51 (routing, nodes — internally parallel)
- **#45:** `currentMaster.distance` is sticky-low → slow failover when only a longer relay path survives. Derive distance from `min(fresh-neighbor distance) + 1`, raising it only once the neighbor that supplied the low value ages out of `NeighborTable`. Review for route oscillation (the documented risk that kept this deferred).
- **#46:** replace `ReplayCache`'s 16-entry exact-tuple ring with a per-origin high-water `(epoch, seq)` — accept only strictly-newer per origin. Memory bounded by peer count, not message rate; closes the early-eviction window. Add a `LATTICE_REPLAY_*` compile-time knob.
- **#51:** `RouteTable` (~2.25 KB) is allocated on every node but used only by the master. Make it a pointer allocated on master-promotion (role read from NVS in `setup()`); leaves allocate nothing. Re-measure via the Phase 0 size job.

### Phase C — #44 (downlink auth, protocol → nodes + hub)
Downlink `route_path` is relay-asserted and unauthenticated (misroute/blackhole DoS). Fix with a **per-hop MAC chain keyed off the existing relay↔master pairwise E2E secret** — chosen because it is robust to node hot-swap (Phase 8 hot-swap is server-only, firmware unchanged: a replacement ESP32 enrolls fresh, the master learns its pubkey, the pairwise secret exists immediately, so the new relay can MAC its hops with zero extra provisioning). No new key material, no server participation, no new persisted state.
- **lattice-protocol:** add a per-hop `route_mac[]` field (firmware-relevant; server tolerates/ignores it like `authTag`). Release + tag as **v4** (flag-day; no-backcompat).
- **nodes:** each relay appends `HMAC(pairwise_secret, hop_context)` to `route_mac` as it rewrites `route_path`; the master verifies every hop against the per-node pairwise secrets it already holds; reject + log on mismatch.
- **hub:** bump `go.mod` protocol pin to v4 + regen `mesh.pb.go`. No verification server-side (the master owns path auth; the hub is a pure downlink broadcaster).

### Phase D — #42 (enrollment harden, nodes)
Enrollment is trust-on-first-use; the master is not cryptographically authenticated. Once Phase H lands, the master's pubkey **is** conveyed in JOIN_ACK (`enrollment_public_key`) — but under plain TOFU it is still unauthenticated (an RF attacker present at enrollment can present their own key as the master).

**Fix: pin the master public key at flash time and verify against it** — true authentication, not window-narrowing. Phase H/#87 gives the hub a durable master keypair (`masterkey.json`), so there is now a stable identity to pin. Since the fleet is reflashed anyway (no-backcompat) and it is a single key shared by all nodes, provisioning is cheap: inject the master pubkey at build/flash time (compile-time constant, or written to NVS at flash — compile-time constant is simplest and needs no storage). In `Enrollment::processJoinAck`, reject any JOIN_ACK whose `enrollment_public_key` does not match the pinned master key.

This was reconsidered from an earlier button-window pick: button-window only *shrinks* the MITM window to the button-press moment and leaves the master key TOFU'd, whereas pinning rejects any wrong key regardless of timing. A server-signed JOIN_ACK was rejected as redundant — the node would still need the master pubkey to verify a signature, so once the key is pinned a direct compare is strictly simpler and gives the same guarantee (there is no key rotation to justify signing).

**Dual-master (#88) trust is transitive:** pin the *primary* master pubkey → the primary's JOIN_ACK is authenticated → the secondary pubkey (proto field 16) arrives *inside* that authenticated frame → it is trusted without a second pin.

Optional: a button-held enrollment-enable gate may still be kept as an operator-convenience control over *when* a node accepts enrollment, but it is **not** the security boundary — pinning is. ~0 server change.

**Depends on Phase H** — not just for a working enrollment, but as a provisioning prerequisite: the hub must have generated its master keypair (#87) before a deployment's nodes are flashed with that pubkey. Also depends on Phase 0's build.

### Phase E — #47 (hygiene, nodes + protocol)
Six low-severity items, none affecting shipped correctness:
1. mbedtls context free-before-`err::fatal` (RAII/scope guard) in `E2ECrypto.h` / `MeshCrypto.h`.
2. Cast `data_type` to `uint32_t` before shifts in `buildAad` (avoid implementation-defined signed shift).
3. Serial relay `proto_version` literal should track `PROTO_VERSION` (`SerialAdapter.cpp:165`).
4. Inline `route_len <= MAX_HOPS` clamp in `sendDownlinkToNode` (defense-in-depth).
5. Note/guard the downlink LRU enrolled/master call-time-only check.
6. lattice-protocol `gofmt -w` on `message/message.go` — fold into Phase C's protocol release.

### Phase F — #63, #64 (hub misc, independent)
- **#63:** dashboard approve posts an empty name → blank NodeCard. Add a name input to the dashboard approval flow (match artist-portal's pattern) **and** default `name` to the MAC string in `AssignNode` as a belt-and-suspenders orchestrator guard. Update `e2e/tests/dashboard/enrollments.spec.ts` (currently asserts `name === ''`).
- **#64:** `meshsim` `writeLocked` does a blocking TCP write while holding the sim mutex → deadlock if the peer stops reading. Add `SetWriteDeadline` in the write path; on deadline error, log + disconnect. Test-infra only.

### Phase G — #52, #53 (optimization, nodes, measurement-gated)
Storage + memory optimization. Context: `docs/memory_usage.md` already holds a prioritized P0–P4 plan; P0/P2/P3-top are covered by Phases 0/A/B (#49/#50/#51). With ESP-IDF now the build (Phase 0), the biggest flash wins are **in scope** — no longer deferred.
- **#52 (flash, the real pressure point):** `CONFIG_BT_ENABLED=n` in `sdkconfig` (~10 KB + heap — now possible under IDF); trim unused mbedtls suites via `mbedtls_config` (10–30 KB, keeping X25519 + the one AEAD); compile out logging `.rodata` (`#if`, not runtime `if`); confirm `-Os` + LTO + `--gc-sections`; single AEAD (drop the unused cipher). Est. ~35–80 KB.
- **#53 (RAM):** shrink `mesh_message` in-RAM residency (242 B × 8-deep ring + per-frame stack copies — coordinate with the wire `static_assert`); right-size `RECV_QUEUE_SIZE`/`CACHE_SIZE`/`LATTICE_ROUTE_TABLE_MAX` after measuring real occupancy. Est. ~1 KB + bounds.

**Gating:** must land after Phase 0 (the size-report job makes every before/after delta real, not estimated) and after A/B (which already bank the EEPROM ceiling + RouteTable win). #53 coordinates with #46 (`CACHE_SIZE`) and #51 (`LATTICE_ROUTE_TABLE_MAX`).

## Deliverable structure

- **This umbrella spec** — committed now; the single cross-repo sequencing + decision record.
- **Per-phase implementation plans** — generated via the writing-plans skill when each phase is picked up, so every phase stays PR-sized and independently executable.
- **First plan:** Phase 0 (ESP-IDF migration), since it gates all firmware build/link, verification, and the memory re-measure.

## Open risks / notes

- **Phase 0 is a prerequisite for measuring anything** — `docs/memory_usage.md` numbers are stale since the #24 restructure + Phases 1–5 and cannot be regenerated until the firmware compiles outside the IDE.
- **Phase C proto release touches three repos** — follow the lattice-protocol release+tag flow; never re-point the nodes submodule at a floating branch SHA.
- **`mesh.pb.*` is hand-edited on the nodes side** (no regen toolchain); add `max_size` options upstream before any regen, or the secondary-master serial codec reverts. The Phase H proto-sync CI check is hub-side (`mesh.pb.go`), separate from this nodes-side fragility.
- **#45 fix must be reviewed for route oscillation** — the naive "latest beacon wins" is explicitly rejected; the freshness-coupled approach is the safe form.

## Session status / resume (2026-07-22)

Where this paused, so a fresh session can pick up without re-deriving.

**Done:** umbrella spec approved; all decisions below locked; issues #52/#53/#54 filed; #49 corrected via comment. Branch `docs/close-all-issues-umbrella-spec` pushed to origin (spec + this doc + memory_usage.md + phase plans; `build/` gitignored).

**Phase 0 research complete — recommendation to confirm:** build via **raw ESP-IDF with arduino-esp32 as a component** (not PlatformIO). Rationale: raw IDF guarantees mbedtls recompiles from source against our `sdkconfig`, so `CONFIG_MBEDTLS_CHACHAPOLY_C=y` deterministically links; the PlatformIO/pioarduino `custom_sdkconfig` path has an unverified gap on exactly that requirement. Version pins: **ESP-IDF v5.5.4**, **arduino-esp32 ^3.3.10** (via `idf_component.yml` / `idf.py add-dependency`), CI **`espressif/esp-idf-ci-action@v1`** running `idf.py build && idf.py size`.

**Verified Phase-0 facts (drove the real toolchain arduino-cli 1.5.1 + esp32@3.3.10):**
- chachapoly needs THREE Kconfig options: `CONFIG_MBEDTLS_CHACHA20_C=y`, `CONFIG_MBEDTLS_POLY1305_C=y`, `CONFIG_MBEDTLS_CHACHAPOLY_C=y` (CHACHAPOLY depends on the other two; all default `n`).
- BT off = `CONFIG_BT_ENABLED=n`; arduino-esp32 does not require BT.
- Node has **no TLS** (`grep` found no `WiFiClientSecure`/HTTPS/`esp_tls`) → mbedtls can trim hard (drop RSA/DHE/ECDSA/SECP256R1, keep X25519 + chachapoly + SHA-256). RSA has no standalone toggle — gated by key-exchange symbols.
- Arduino API surface is all core-provided (String, EEPROM, `esp_now_`, Serial, digitalWrite, WiFi) → arduino-as-component keeps them; migration is mechanical (.ino→`main/main.cpp` + forward decls, `CONFIG_AUTOSTART_ARDUINO=y` or `initArduino()`, `CMakeLists.txt` with `SRC_DIRS "." "src"`, `partitions.csv`, `CONFIG_FREERTOS_HZ=1000`).
- **EEPROM lib gotcha:** under ESP-IDF the Arduino `EEPROM` lib needs a custom partition (type `data`, subtype `0x99`, label `eeprom`) or `EEPROM.begin()` silently fails. **#50's NVS/Preferences migration removes this need** — EEPROM is confined to `EepromManager.{h,cpp}` + `Mesh.cpp`.
- `idf.py` is NOT installed locally → Phase 0 build is only verifiable in CI or after a local IDF 5.5.4 install, not in the planning session.

**Two decisions still open (needed before the Phase 0 implementation plan):**
1. **Build tool** — confirm raw ESP-IDF (recommended) vs PlatformIO (pioarduino; would add a must-pass spike proving `custom_sdkconfig` rebuilds mbedtls).
2. **EEPROM/#50 sequencing** — add a temporary `0x99` `eeprom` partition in Phase 0 (keep #50 in Phase A, phases stay independent) **vs** pull #50 (EEPROM→Preferences) into Phase 0 to avoid the partition workaround entirely.

**Next action on resume:** answer the two decisions, then invoke `superpowers:writing-plans` for Phase 0 (ESP-IDF migration), saved to `docs/superpowers/plans/2026-07-22-phase0-esp-idf-migration.md`.
