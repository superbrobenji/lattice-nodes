# Phase G — Memory optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Land all Phase G scope from `docs/superpowers/specs/2026-08-04-phaseG-memory-opt-design.md` (flash + RAM levers + wire shrink + audit items A-Q). Cross-repo — protocol v0.6.0 flag-day.

**Architecture:** Split into 7 tasks. Task 1 tags protocol v0.6.0. Tasks 2/3/5 land nodes-only trims + refactors independently. Task 4 bumps nodes submodule + wire-consumer changes (needs v0.6.0). Task 6 bumps hub go.mod + ProtoVersion 4→5 (needs v0.6.0). Task 7 verifies + closes.

**Tech Stack:** Go 1.21 (protocol), C++ (ESP-IDF), Go 1.23 (hub), GoogleTest, Playwright.

## Global Constraints

- Flag-day, no backwards compat with protocol v0.5.0. `ProtoVersion` bumps 4→5 atomically at hub+nodes merge.
- `WireSize` == 200B post-Phase G (static_assert enforced).
- Design doc canonical: `docs/superpowers/specs/2026-08-04-phaseG-memory-opt-design.md`.
- Audit ledger: `docs/superpowers/specs/2026-08-04-post-phaseG-audit-findings.md`.
- Umbrella "release-flow rule": protocol tag pushed BEFORE nodes/hub PRs open. Nodes+hub PRs merge together.
- Under `UNIT_TEST`, `err::fail` throws `FatalError`.

---

### Task 1: Protocol v0.6.0 — wire shrink + regen (lattice-protocol)

**Repo:** `/Users/benji/projects/personal/lattice-protocol` — branch `feat/phaseG-wire-shrink-v0.6.0`.

**Files:**
- Modify: `message/message.go` — drop `SecondaryMasterMac` + `SecondaryPublicKey` fields (proto tags 15+16); shrink `RoutePath [60]byte → [48]byte`. Update `WireSize` constant `250 → 200`.
- Modify: `proto/mesh.options` — nanopb `max_size` for `routePath` 60→48; drop options for the two removed fields.
- Regen: `c/mesh_message.h` (`WireSize=200`, `static_assert` updated); `proto/mesh.proto`.
- Modify: `README.md` — versioning table row for v0.6.0.

**Interfaces produced:**
- `MeshMessage.WireSize == 200`.
- `MeshMessage.SecondaryMasterMac` + `SecondaryPublicKey` REMOVED. Consumers must pack/unpack from `data[4..42]` in JOIN_ACK frames.
- `MeshMessage.RoutePath` = 48 bytes (max 8 hops × 6 bytes).

- [ ] **Step 1:** Edit `message/message.go` — remove the two `Secondary*` field lines; change `RoutePath` array size; update `WireSize` const.
- [ ] **Step 2:** Edit `proto/mesh.options` — update `routePath max_size:48`; delete `secondaryMasterMac` + `secondaryPublicKey` size options.
- [ ] **Step 3:** `go generate ./...` — regenerate `c/mesh_message.h` + `proto/mesh.proto`.
- [ ] **Step 4:** `make check` — verify zero drift after regen.
- [ ] **Step 5:** `go test ./...` — all packages pass.
- [ ] **Step 6:** Add v0.6.0 row to `README.md` versioning table: "Wire shrink 250→200B: dropped top-level Secondary{MasterMac,PublicKey} (moved to JOIN_ACK data payload); `RoutePath[60→48]` (`MAX_HOPS` 10→8). Flag-day, no v5 backcompat."
- [ ] **Step 7:** Commit + push + open PR:

```bash
git add message/message.go proto/mesh.options c/mesh_message.h proto/mesh.proto README.md
git commit -m "feat: protocol v0.6.0 — wire shrink 250→200B (Phase G)

Drops top-level SecondaryMasterMac + SecondaryPublicKey (moved to
JOIN_ACK data payload by consumers; wire savings −38B on every frame).
Reduces RoutePath[60→48] via MAX_HOPS 10→8 (−12B).

WireSize 250 → 200. ESP-NOW headroom restored to 50B. Flag-day; bumps
protocol to v5 (nodes+hub PROTO_VERSION coordination).

Part of lattice-nodes Phase G (issues #52 + #53)."
git push -u origin feat/phaseG-wire-shrink-v0.6.0
gh pr create --title "feat: protocol v0.6.0 — wire shrink 250→200B (Phase G)" --body "Phase G wire shrink. Merge, then tag v0.6.0. Nodes + hub bumps depend on the tag."
```

- [ ] **Step 8: HANDOFF** — after PR merge, tag + push:

```bash
git checkout main && git pull --ff-only
git tag -a v0.6.0 -m "v0.6.0: protocol v5 wire shrink 250→200B (Phase G)"
git push origin v0.6.0
```

---

### Task 2: Nodes flash trim (nodes, standalone)

**Repo:** `/Users/benji/projects/personal/lattice-nodes` — branch `feat/phaseG-flash-trim`.

Bundles: log-macro gating (§1), sdkconfig `-Os`/LTO verify (§2), mbedtls AES/GCM/CCM trim (§3), audit A (dead code), E (FF:FF broadcast), J (pin-mask), M (SevenSeg dedup), N (readOwnMac helper).

**Files:**
- Modify: `firmware/main/src/logging/Logger.h` — add `LATTICE_LOG` / `LATTICE_LOGLN` macros gated by `LATTICE_DEFAULT_LOG_LEVEL`.
- Modify: `firmware/main/project_config.h` — add `LATTICE_DEFAULT_LOG_LEVEL` compile-time integer mirroring runtime enum.
- Modify: `firmware/sdkconfig.defaults` — add/verify `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, `CONFIG_COMPILER_OPTIMIZATION_LTO=y`, `CONFIG_MBEDTLS_AES_C=n`, `CONFIG_MBEDTLS_GCM_C=n`, `CONFIG_MBEDTLS_CCM_C=n`.
- Modify: ~100 `Logger::log*` call sites — mechanical rewrite to `LATTICE_LOG*` macros.
- Delete: `Mesh.cpp` `printMac`, `printMeshMessage`, `generateRandomMeshKey`, `meshKeyIsSet` blocks; declarations in `Mesh.h`.
- Delete: `Adapter.cpp` LED stub block + `LED_ADAPTER_DEFAULT_PIN`; `RELAY_ADAPTER` enum value (verify no references first with `grep -rn "RELAY_ADAPTER" firmware/main/`).
- Delete: `Error.h:88-97` `ERROR_ASSERT` + `ERROR_CHECK` templates.
- Delete: `MacAddress.h` `MacAddress(const String&)` ctor.
- Create: `firmware/main/src/mesh/broadcast_mac.h` — single `constexpr uint8_t BROADCAST_MAC[6] = {0xFF,...}`; consolidate 7 sites in `Mesh.cpp` + `Enrollment.cpp:72`.
- Create: `firmware/main/src/network/hw_mac.h` — one `readOwnMac(uint8_t[6])` helper; delete 3 duplicates.
- Modify: `GpioInput.cpp:19-46` + `GpioOutput.cpp:19-42` — `switch` → `constexpr uint64_t VALID_MASK`.
- Modify: `SevenSegDisplay.cpp:180-244` — `show()` + `showWithDP()` → `showInternal(value, leadingZeros, bool withDP)`.

**Interfaces produced:**
- `LATTICE_LOG(tag, msg, level)`, `LATTICE_LOGLN(tag, msg, level)` — no-op in prod (level=NONE), route to Logger otherwise.
- `lattice::mesh::BROADCAST_MAC` — canonical.
- `lattice::hw::readOwnMac(uint8_t[6])` — canonical.

- [ ] **Step 1:** Add macros to `Logger.h` (per design §1).
- [ ] **Step 2:** Update `project_config.h` with `LATTICE_DEFAULT_LOG_LEVEL`.
- [ ] **Step 3:** Grep all `Logger::log`/`Logger::logln` call sites, rewrite to `LATTICE_LOG`/`LATTICE_LOGLN`.
- [ ] **Step 4:** Update `sdkconfig.defaults` with `-Os`/LTO/mbedtls trim knobs. Try build; if mbedtls trim breaks link, roll back specific offending knob.
- [ ] **Step 5:** Delete dead code per file list above.
- [ ] **Step 6:** Create `broadcast_mac.h` + `hw_mac.h`; replace duplicates.
- [ ] **Step 7:** Rewrite pin-validation `switch` → mask in Gpio{Input,Output}.cpp.
- [ ] **Step 8:** Refactor SevenSegDisplay `show`/`showWithDP` → `showInternal`.
- [ ] **Step 9:** Full unit + e2e suite green.
- [ ] **Step 10:** Commit + push + PR:

```bash
git commit -m "feat(phaseG): flash trim — log macros, mbedtls trim, dead-code delete, DRY

Flash levers 1-3 (log-.rodata compile-time gate, -Os/LTO verify,
mbedtls AES/GCM/CCM off) + audit items A/E/J/M/N.

Est. ~10-25 KB flash saved. CI size job attached below.

Part of Phase G (issues #52, #53)."
```

---

### Task 3: Nodes RAM residency + CompactMessage (nodes)

**Branch:** `feat/phaseG-ram-residency` (or continue same feat branch — implementer's choice; separate branch keeps PR review manageable).

Bundles: RECV_QUEUE_SIZE (§4), ROUTE_TABLE_MAX (§5), REPLAY_MAX_ORIGINS (§6), CompactMessage (§7), audit B (E2E_KEYCACHE role split), C (PENDING_RELAY_QUEUE follow), D (ReplayCache padding), K (PIR state), L (pin types).

**Files:**
- Modify: `firmware/main/project_config.h` — bound tunings + `LATTICE_E2E_KEYCACHE_MAX` role-conditional split (either two constants or a runtime cap driven by `isMaster`).
- Modify: `firmware/main/src/mesh/ReplayCache.h` — field reorder for padding.
- Modify: `firmware/main/src/mesh/E2EKeyStore.h` — role-conditional cache size.
- Modify: `firmware/main/src/mesh/Enrollment.h` — `PENDING_RELAY_QUEUE_SIZE = 4` (mirrors new RECV_QUEUE_SIZE).
- Create: `firmware/main/src/mesh/CompactMessage.{h,cpp}` — new compact struct + `toCompact`/`toWire` converters.
- Modify: `firmware/main/src/mesh/Mesh.h` — `recvQueue` element type → `CompactMessage`.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — decode/encode boundary in `messageProcessor` + `drainRecvQueue`; extract type-specific fields from raw wire buffer when needed (JOIN_ACK secondary fields, RouteReport auth_path).
- Modify: `firmware/main/src/adapter/pir/PirAdapter.h` — `_cooldownSeconds` → `constexpr`; collapse `_timerActive`/`_motionSent`.
- Modify: `firmware/main/src/adapter/Adapter.h` + `AdapterFactory.*` — pin types `int` → `uint8_t`.
- Test: `tests/unit/test_compact_message.cpp` (new) — round-trip preservation.

- [ ] **Step 1:** Write failing round-trip tests for CompactMessage.
- [ ] **Step 2:** Implement CompactMessage + converters.
- [ ] **Step 3:** Tune bounds in project_config.h.
- [ ] **Step 4:** ReplayCache padding reorder.
- [ ] **Step 5:** E2E_KEYCACHE role split.
- [ ] **Step 6:** PENDING_RELAY_QUEUE_SIZE follow.
- [ ] **Step 7:** PIR state cleanup + pin type shrink.
- [ ] **Step 8:** Change `recvQueue` element type + adapt Mesh.cpp decode boundary.
- [ ] **Step 9:** Full unit + e2e suite green.
- [ ] **Step 10:** Commit + push + PR.

---

### Task 4: Nodes wire consumer changes (needs Task 1's v0.6.0 tag)

**Branch:** `feat/phaseG-wire-consumer`.

**Precondition:** protocol `v0.6.0` tag exists on origin.

**Files:**
- Modify: `firmware/main/lib/lattice-protocol` submodule pin → v0.6.0.
- Modify: `firmware/main/src/mesh/Enrollment.cpp::processJoinAck` — read secondary from `data[4..42]` (post-pin verify, pre-registration).
- Modify: `firmware/main/project_config.h` — `MAX_HOPS = 8`.
- Modify: `firmware/main/src/mesh/Mesh.h` — `PROTO_VERSION = 5`.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — verify `route_len ≤ MAX_HOPS` (Phase E clamp already handles).
- Update: `tests/unit/test_enrollment.cpp` (or `test_mesh_logic.cpp`) — dual-master JOIN_ACK reads secondary from `data[]`.

- [ ] **Step 1:** Bump submodule to v0.6.0; verify `c/mesh_message.h` reflects 200B.
- [ ] **Step 2:** Update `Enrollment::processJoinAck` — parse secondary from data payload.
- [ ] **Step 3:** Update `MAX_HOPS` = 8 in project_config.h.
- [ ] **Step 4:** Bump `PROTO_VERSION` = 5.
- [ ] **Step 5:** Update tests for new dual-master layout.
- [ ] **Step 6:** Full regression + e2e.
- [ ] **Step 7:** Commit + push + PR:

```bash
git commit -m "feat(phaseG): consume protocol v0.6.0 wire shrink + PROTO_VERSION 5

Bumps submodule to v0.6.0 (wire 200B). Enrollment::processJoinAck
reads secondary master MAC + pubkey from data[4..42] (previously
top-level MeshMessage fields, now packed into JOIN_ACK data payload).
MAX_HOPS 10→8. PROTO_VERSION 4→5 atomic flag-day (hub bumps in
parallel PR)."
```

---

### Task 5: Nodes DRY + robustness (audit F/G/H/I/O/P/Q)

**Branch:** `feat/phaseG-audit-dry`.

- Item F: cache `esp_wifi_get_mac` boot-time result in `g_deviceMac`; replace 4-6 per-RX-frame syscall sites.
- Item G: cache `Enrollment::isEnrolled()` state — set in `init`/`processJoinAck`/`saveEnrolledFlag`.
- Item H: `std::function` → function-pointer typedefs on `externalRecvCallback`, `RegisterPeerFn`, `EnrollmentRelayFn`.
- Item I: drop `virtual` on GpioInput/GpioOutput `init()`.
- Item O: extend `_persistOrEscalate` to all 18 NVS put-sites (13 non-security get `securityRelevant=false`).
- Item P: extend `MbedtlsGuard.h` with `EcpGroupCtx`, `MpiCtx`, `EcpPointCtx`, `ChaChaPolyCtx`; migrate `MeshCrypto.h`/`E2ECrypto.h` sites.
- Item Q: canonical `bool lattice::mac::eq(const uint8_t*, const uint8_t*)`; grep both existing idioms (`memcmp(...,6)==0` and `MacAddress == MacAddress`), replace all ~60 sites.

- [ ] Each item = one commit; full regression per item.
- [ ] Final PR bundles all 7 items with size delta in body.

---

### Task 6: Hub v0.6.0 + ProtoVersion 5 + JOIN_ACK data-payload write (needs Task 1's tag)

**Repo:** `/Users/benji/projects/personal/lattice-hub` — branch `chore/phaseG-protocol-v0.6.0`.

**Precondition:** protocol `v0.6.0` tag exists.

**Files:**
- Modify: `server/orchestrator/go.mod` — protocol v0.5.0 → v0.6.0; `go mod tidy`.
- Regen: `server/orchestrator/mesh/mesh.pb.go` via `go generate ./mesh/...`.
- Modify: `server/orchestrator/mesh/server.go::ApproveEnrollment` — pack secondary MAC + pubkey into `data[4..42]` of JOIN_ACK's data field when secondary is configured; drop the top-level `SecondaryMasterMac`/`SecondaryPublicKey` writes (fields no longer exist in v0.6.0).
- Modify: `server/orchestrator/mesh/server.go` — every `ProtoVersion` gate + outbound stamp bumps `4 → 5` atomically.
- Update: test fixtures — JOIN_ACK secondary read from `data[]` layout.

- [ ] **Step 1:** Sync main; branch.
- [ ] **Step 2:** `go get lattice-protocol@v0.6.0`; `go mod tidy`; `go generate ./mesh/...`.
- [ ] **Step 3:** Update `ApproveEnrollment` — pack secondary into `data[4..42]`.
- [ ] **Step 4:** Bump `ProtoVersion` 4→5 at every hit (grep `ProtoVersion` production + tests).
- [ ] **Step 5:** `go test -race -count=1 ./...`.
- [ ] **Step 6:** Commit + push + PR.

---

### Task 7: Cross-repo verify + close #52, #53

- [ ] Confirm protocol v0.6.0 tag pushed.
- [ ] Confirm nodes PRs (Tasks 2/3/4/5) + hub PR (Task 6) merged in correct order (Tasks 4+6 flag-day pair).
- [ ] Post CI size deltas in each PR body.
- [ ] `Closes #52, #53` fires on the nodes PR containing the flash+RAM items.
- [ ] SDD ledger.

---

## Execution notes

- Tasks 2, 3, 5 are nodes-only + parallelizable; each is its own PR.
- Tasks 4 + 6 are the flag-day pair — must merge together.
- Task 1 is the release-flow gate — its tag must exist before Tasks 4 + 6 open PRs.
- Suggested subagent-driven execution order: Task 1 → Task 2 → Task 3 → Task 5 → (Task 4 + Task 6 parallel setup, sequential dispatch) → Task 7.

## Self-review

Coverage: §1 → Task 2, §2 → Task 2, §3 → Task 2, §4-§7 → Task 3, §8 → Tasks 1+4+6, §9 (A-Q) → Tasks 2 (A/E/J/M/N), 3 (B/C/D/K/L), 5 (F/G/H/I/O/P/Q), §10 CI size → Task 7 aggregate.
