# Phase C — Downlink auth (#44)

**Status:** Approved
**Date:** 2026-08-04
**Scope:** cross-repo — `lattice-protocol` (wire), `lattice-nodes` (firmware), `lattice-hub` (dep bump only).
**Parent:** `docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase C).
**Related:** `lattice-nodes` issue #44.

## Context

Phase 3 downlink source routing accumulates the relay path in the plaintext `route_path[]`/`route_len` header fields — deliberately excluded from AEAD AAD so relays can rewrite them in flight. The path a master learns (and downlink follows) is relay-asserted, not authenticated. A malicious/compromised relay can report a false path; the master's `RouteTable` learns bogus routes; downlink to a targeted node is misrouted / blackholed. Payload confidentiality/integrity is unaffected — AEAD still gates content and target-MAC binding — so the risk is DoS-class within the accepted local-attacker envelope. Umbrella Phase C escalates from "accepted" to "authenticated" now that Phase 0 + Phase A + Phase B are done.

Approach: per-hop **chained** MAC keyed off the existing relay↔master pairwise E2E secret (`k_up`). No new key material, no server participation, no new persisted state. Chosen because it is robust to node hot-swap (Phase 8 hot-swap is server-only; a replacement ESP32 enrolls fresh, master learns its pubkey, `k_up` exists immediately, new relay can MAC its hops with zero extra provisioning).

## Design

### 1. Wire (`lattice-protocol` v0.5.0, flag-day no-backcompat)

Add one new field to `MeshMessage`:

```go
// message/message.go
AuthPath [8]byte `c:"uint8_t[8]" proto:"17,bytes,optional,authPath"`
```

Truncated to 8 bytes (64-bit security). Chained MAC — single field, not per-hop array — so wire cost is fixed regardless of `MAX_HOPS`. `WireSize` becomes 242 + 8 = **250 bytes**, exactly at the ESP-NOW payload cap. Codegen's `static_assert(sizeof(mesh_message) == 250, ...)` enforces the fit.

Existing `route_path[60]`, `route_len`, `auth_tag[16]`, `secondary_master_mac`, `secondary_public_key[32]` unchanged.

Tag `v0.5.0` after merge. This is the umbrella's `v4` wire version bump — flag-day: `ProtoVersion` bumps to 4; older firmware/hub rejects.

### 2. Nodes (`lattice-nodes`)

**New header** `firmware/main/src/mesh/RouteMac.h` (header-only, matches `E2ECrypto.h` pattern):

```cpp
namespace lattice::mesh::routemac {

// hop_context (30 bytes):
//   origin_mac(6) || dest_mac(6) || epoch(4LE) || seq(2LE) || prev_hop_mac(6) || this_hop_mac(6)
// prev_hop_mac is zeroed for the originating hop.
constexpr size_t HOP_CTX_LEN = 30;
constexpr size_t AUTH_PATH_LEN = 8;

void buildHopContext(const mesh_message& msg, const uint8_t prev_hop[6],
                     const uint8_t this_hop[6], uint8_t out_ctx[HOP_CTX_LEN]);

// mac_i = HMAC-SHA256(secret, hop_context_i || mac_{i-1})[:8]
// For the originating hop, prev_mac must be zeroed 8B.
void chainStep(const uint8_t secret[32], const uint8_t hop_ctx[HOP_CTX_LEN],
               const uint8_t prev_mac[AUTH_PATH_LEN], uint8_t out_mac[AUTH_PATH_LEN]);

}
```

Implementation uses `mbedtls_md_hmac(MBEDTLS_MD_SHA256, ...)` — already linked (Phase A wired HKDF-SHA256).

**Uplink route-report accumulation** — two edit points:

1. `Mesh::originateRouteReport` (or wherever the node builds its own OpRouteReport frame): before send, compute `mac_0 = chainStep(k_up_with_master, ctx_0, zeros)`, write to `msg.auth_path`.
2. Each relay path in `Mesh` that rewrites `route_path` on forward: compute `mac_i = chainStep(k_up_with_master, ctx_i, msg.auth_path)`, overwrite `msg.auth_path`.

Both edits happen inside master-MAC-known code paths (relays already hold their own `k_up` with the master via existing `E2EKeyStore` after enrollment).

**Master verification** in `Mesh::processRouteReport`, before existing `routes->record(...)` call:

```cpp
uint8_t computed[routemac::AUTH_PATH_LEN] = {0};
uint8_t prev_hop[6] = {0};
for (size_t i = 0; i < msg.route_len; ++i) {
    const uint8_t* hop_mac = &msg.route_path[i * 6];
    uint8_t k_up[32];
    if (!e2eKeys.lookupUp(hop_mac, k_up)) {
        Logger::logln("MESH", "Route report from unknown hop; drop", LogLevel::LOG_ERROR);
        return;
    }
    uint8_t ctx[routemac::HOP_CTX_LEN];
    routemac::buildHopContext(msg, prev_hop, hop_mac, ctx);
    routemac::chainStep(k_up, ctx, computed, computed);
    memcpy(prev_hop, hop_mac, 6);
}
if (memcmp(computed, msg.auth_path, routemac::AUTH_PATH_LEN) != 0) {
    Logger::logln("MESH", "Route report MAC verify failed; drop", LogLevel::LOG_ERROR);
    return;
}
routes->record(...);  // only on verify pass
```

`e2eKeys.lookupUp(mac, out_key)` is the existing master-side accessor on `E2EKeyStore` (verify exact API name at implementation time; may be `getUp` / `deriveUp` etc.).

### 3. Hub (`lattice-hub`)

Purely a dependency bump. No orchestrator code change.

- `server/orchestrator/go.mod`: `github.com/superbrobenji/lattice-protocol` v0.4.2 → v0.5.0.
- `go generate ./mesh/...` regenerates `server/orchestrator/mesh/mesh.pb.go` (proto-sync CI job verifies drift).

The `AuthPath` field is opaque bytes on the hub — the orchestrator forwards downlinks to the firmware over serial and does not participate in authentication. `ProtoVersion` gate in the hub already accepts inbound `ProtoVersion == 3`; bump to `== 4` on this merge to reject old firmware in one atomic step (part of the flag-day rule).

## Cross-repo release-flow (mandatory order)

Per umbrella's "release-flow rule": never merge nodes/hub against a floating protocol branch SHA.

1. **Protocol PR** — add `AuthPath`, regen, `make check` clean, merge.
2. **Protocol tag** `v0.5.0`, push tag.
3. **Nodes PR** — bump submodule to `v0.5.0`, add `RouteMac.h`, wire accumulation + verification, tests. Depends on (2).
4. **Hub PR** — bump `go.mod` to `v0.5.0`, regen `mesh.pb.go`, bump `ProtoVersion` gate 3 → 4. Depends on (2).

Nodes + Hub PRs can open in parallel after step 2. Neither can merge before step 2 is complete.

## Error handling

- Verify fail: drop frame, `Logger::logln(..., LOG_ERROR)`. No `err::fail` — misroute is DoS-class per issue #44, halting the master would amplify DoS. No RouteTable mutation on fail.
- Unknown hop: same — drop, log, no mutation.
- `mbedtls_md_hmac` failure: cannot happen for the SHA-256 setup path in practice; if it did, treat as verify fail (drop + log).

## Testing

**Protocol:**
- Codegen produces `WireSize == 250`; existing `WireSize` assertions updated.
- `make check` (codegen ↔ committed diff) clean.

**Nodes:**
- `test_route_mac.cpp` (new):
  - `BuildHopContext_ByteExact` — fixed inputs → fixed 30-byte output.
  - `ChainStep_StableAcrossReboots` — same secret + context + prev → same output.
  - `ChainStep_ChangesWithAnyInputBit` — sensitivity per input field.
- `test_route_report.cpp` (extend):
  - `MasterVerifiesValidChain_RecordsPath` — single-hop and 3-hop valid chains both accept.
  - `MasterRejectsTamperedRoutePath_DropsNoRecord` — flip one byte in `route_path`, verify chain no longer matches, `RouteTable` untouched.
  - `MasterRejectsTamperedAuthPath_DropsNoRecord` — flip one byte in `auth_path`, same.
  - `MasterRejectsUnknownHop_DropsNoRecord` — insert a hop MAC not in `E2EKeyStore`.
- `test_route_report_e2e.cpp` (extend):
  - `RouteReportEndToEnd_ValidChain` — sim network, legit chain accumulates through relays, master records path.
  - `RouteReportEndToEnd_TamperedByEvilRelay` — sim relay flips a bit before forwarding; master drops, `RouteTable` unchanged.

**Hub:** existing tests + `proto-sync` CI catch drift. No new tests.

## Non-goals

- No downlink-frame MAC (only route-report accumulation is authenticated; downlink frames use the master-verified `RouteTable` directly).
- No hub-side verification (hub is pure broadcaster per umbrella).
- No new persisted state on any node.
- No backwards compatibility with protocol v3 / v0.4.x — flag-day, per umbrella "no backcompat" rule.
- Not fixing #47 (hygiene) or #52/#53 (memory) items adjacent to touched code.

## Files touched (estimate)

**lattice-protocol:**
- `message/message.go` (+1 field)
- `c/mesh_message.h`, `proto/mesh.proto` (regen)
- README versioning table

**lattice-nodes:**
- `firmware/main/lib/lattice-protocol` submodule pin bump
- `firmware/main/src/mesh/RouteMac.h` (new, header-only)
- `firmware/main/src/mesh/Mesh.cpp` — accumulation + verification (~30 LOC)
- `firmware/main/src/mesh/Mesh.h` — no signature changes expected
- `tests/unit/test_route_mac.cpp` (new)
- `tests/unit/test_route_report.cpp` — extend
- `tests/e2e/scenarios/test_route_report_e2e.cpp` — extend

**lattice-hub:**
- `server/orchestrator/go.mod`, `go.sum`
- `server/orchestrator/mesh/mesh.pb.go` (regen)
- `server/orchestrator/mesh/server.go` — `ProtoVersion` gate 3 → 4 (1-line change)

Rough size: ~40 LOC protocol, ~200 LOC nodes production, ~350 LOC nodes tests, ~10 LOC hub.
