# Umbrella Design: Close All Open Issues (lattice-nodes + lattice-hub + lattice-protocol)

**Date:** 2026-07-22
**Status:** Approved (umbrella) — per-phase implementation plans generated on pickup
**Scope:** 17 open issues across three repos, decomposed into 10 sequenced phases.

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
| 0 — Build unblock | #49 | nodes | Fix arduino-cli compile (3 blockers) + add CI firmware-compile/size job |
| H — Hub enrollment | #87, #85, #86 | hub | Master keypair → correct JOIN_ACK → fix test. P0: enrollment is broken today |
| H2 — Hub dual-master | #88 | hub | Populate secondary-master fields in JOIN_ACK (hub half of shipped firmware Phase 4/5) |
| A — Persistence | #50, #43 | nodes | Migrate EepromManager → NVS/Preferences (clean start); fix DEV_MODE epoch + commit checks |
| B — Routing | #45, #46, #51 | nodes | Distance-freshness coupling; replay high-water; RouteTable role-gating |
| C — Downlink auth | #44 | protocol → nodes + hub | Per-hop HMAC-authenticated route_path; flag-day v4 proto field |
| D — Enrollment harden | #42 | nodes | Button-press pairing window (TOFU-in-window) |
| E — Hygiene | #47 | nodes + protocol | 6 code-hygiene items + lattice-protocol `gofmt` |
| F — Hub misc | #63, #64 | hub | Empty-name enrollment default; meshsim write-under-mutex deadlock |
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
```

- **Phase 0 and Phase H are in different repos with no shared dependency** — plan/execute concurrently.
- **Phase B and Phase F are independent** — parallelizable.
- **Phase C is the only multi-repo release.** Order: lattice-protocol change → release + tag → lattice-nodes submodule re-point + firmware → lattice-hub `go.mod` bump + `mesh.pb.go` regen. Never merge nodes against a floating protocol branch SHA (release-flow rule).

## Per-issue approach (locked decisions)

### Phase 0 — #49 (build unblock, nodes)
Firmware does not compile under `arduino-cli` today; only the Arduino IDE's include resolution masks it. Three blockers:
1. **nanopb include path** — add `main/src/mesh/serialization/nanopb` to the sketch include path (build property).
2. **Non-self-contained headers** — `ButtonHandler.h` (and siblings) use unqualified `Logger`/`LogLevel` relying on `main.ino`'s `using namespace`. Each header `#include`s what it uses and fully-qualifies `lattice::utils::Logger` / `lattice::utils::LogLevel`. Sweep for the same pattern (overlaps #47).
3. **Duplicate `mesh_message` typedef** — keep vendored `main/lib/lattice-protocol/c/` out of the Arduino `lib/` auto-scan (collides with `using ::mesh_message`).

Then add a CI job: `arduino-cli compile --fqbn esp32:esp32:esp32 main` + `xtensa-esp32-elf-size` size report that regenerates `docs/memory_usage.md`. **Gate:** green firmware build required before any later firmware phase.

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
Enrollment is trust-on-first-use; the master is not cryptographically authenticated. Note: once Phase H lands, the master's pubkey **is** conveyed in JOIN_ACK — but it is still unauthenticated (nothing signs that it is the real master's key). Mitigation: gate JOIN_ACK acceptance on a **physical button-held pairing window** (`ButtonHandler`); outside the window, drop TOFU master-learn. Document as the accepted "TOFU-in-window" trust model. ~0 server change. Depends on Phase H (a working enrollment to gate) and Phase 0's build.

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

## Deliverable structure

- **This umbrella spec** — committed now; the single cross-repo sequencing + decision record.
- **Per-phase implementation plans** — generated via the writing-plans skill when each phase is picked up, so every phase stays PR-sized and independently executable.
- **First plan:** Phase 0 (build unblock), since it gates all firmware verification and the memory re-measure.

## Open risks / notes

- **Phase 0 is a prerequisite for measuring anything** — `docs/memory_usage.md` numbers are stale since the #24 restructure + Phases 1–5 and cannot be regenerated until the firmware compiles outside the IDE.
- **Phase C proto release touches three repos** — follow the lattice-protocol release+tag flow; never re-point the nodes submodule at a floating branch SHA.
- **`mesh.pb.*` is hand-edited on the nodes side** (no regen toolchain); add `max_size` options upstream before any regen, or the secondary-master serial codec reverts. The Phase H proto-sync CI check is hub-side (`mesh.pb.go`), separate from this nodes-side fragility.
- **#45 fix must be reviewed for route oscillation** — the naive "latest beacon wins" is explicitly rejected; the freshness-coupled approach is the safe form.
