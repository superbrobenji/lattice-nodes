<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Design: Multi-Hop Routing with End-to-End Payload Crypto (Protocol v3)

**Status:** Approved 2026-07-16.
**Closes:** Design gaps #7 (multi-hop data uplink) and #8 (dual-master data
failover) — see `docs/design-gaps/multihop-data-uplink.md`.
**Executable specs:** the two `DISABLED_` tests in
`tests/e2e/scenarios/test_multihop_e2e.cpp` and
`tests/e2e/scenarios/test_dual_master_e2e.cpp` become enabled as their phases
land.

## Decisions (from brainstorm)

| Question | Decision |
|---|---|
| Threat model | **High** — end-to-end payload crypto; relays forward ciphertext they cannot read |
| Scope | Full routing overhaul: uplink (#7), downlink commands, dual-master data failover (#8) |
| Compatibility | Flag-day reflash; protocol v3, no mixed-version support. v3 nodes drop `proto_version != 3` (existing check, `Mesh.cpp:247`) |
| Scale | Configurable at compile time; deployments may be large. Relays must stay stateless |
| Approach | **A** — E2E AEAD + source-routed downlink (per-hop link crypto rejected: ESP-NOW caps encrypted peers at ~6–7; group key rejected: no compromise containment) |

Hardware fact underpinning the approach: ESP-NOW supports ~6–7 **encrypted**
peers but 20 unencrypted peers per node, and broadcast needs no peer entry.
E2E payload crypto makes hop encryption unnecessary, so forwarding can use
unencrypted peer slots and scale.

## 1. Protocol v3 wire format

Header stays plaintext (relays need it to forward):

```
proto_version=3 | message_type | origin_mac(6) | target_mac(6) | last_hop_mac(6)
| hop_count(u8) | epoch_num(u32) | seq_num(u16)
| route_len(u8) + route_path[route_len × 6B]    // NEW; downlink source route,
                                                 //      or uplink path accumulator
| payload ciphertext + 16B Poly1305 tag          // NEW; sealed
```

- Payload (adapter data, commands, route reports) is ChaCha20-Poly1305
  (mbedtls) ciphertext.
- **AAD** = immutable header fields: version, type, origin, target, epoch,
  seq. Mutable-in-flight fields (last_hop, hop_count, route_path) are
  excluded — relays legally rewrite them. Tampering with excluded fields
  yields at worst misrouting/drop (DoS equivalent to jamming), never payload
  disclosure or forgery.
- Beacons, enrollment requests, and JOIN_ACK remain plaintext broadcast as
  today; the TOFU + enrollment-pubkey model is unchanged.

## 2. Keying and crypto

- **E2E keys per (node, master) pair:** HKDF-SHA256 over the existing
  Curve25519 ECDH shared secret (pubkeys already exchanged at enrollment)
  derives **two** 32-byte keys, `k_up` and `k_down`, with distinct HKDF
  labels. Direction-split keys eliminate cross-direction nonce collisions.
- **Nonce (12B):** `epoch(4) || seq(2) || origin_mac(6)`.
  - Epoch is the EEPROM-persisted boot counter (`Mesh.cpp:120-122`);
    seq is per-boot. Unique per origin per key.
  - **Mandatory guard — seq wrap:** u16 seq overflows after 65,535 messages
    in one boot. On wrap: increment and persist epoch, reset seq to 0.
    Without this, a long-lived node reuses nonces (fatal for AEAD).
- **ESP-NOW layer:** shared PMK (`DEFAULT_MESH_KEY`) kept as cheap outer
  layer. Per-peer LMK encryption is **dropped for the data path** — E2E AEAD
  is the security boundary. Data frames travel as unencrypted ESP-NOW
  unicast to auto-added forwarding neighbors (20-peer cap, LRU-evicted).
  The per-peer LMK derivation path (`MeshCrypto::derivePeerLMK`,
  `registerPeerWithEspNow` encrypt branch) becomes dead code and is removed
  in Phase 1; the ECDH keypair and pubkey exchange remain — they now feed
  HKDF instead.
- **Trust split:**
  - `PeerRegistry` — identity + key material. The enrollment-only add rule
    (`PeerRegistry.cpp:67`) is **unchanged**.
  - `NeighborTable` (new) — unauthenticated forwarding candidates learned
    from beacons. Routing only; never holds or implies key material.
  - Accepted risk: a spoofed beacon can attract (blackhole) traffic — a
    local-attacker DoS comparable to RF jamming. It cannot read or forge
    payloads. Documented, not mitigated in this cycle.

## 3. Uplink routing — NeighborTable

Replaces the single `currentMaster.nextHop` as route source.

- Entry: `mac(6) | masterDistance(u8) | lastSeenMillis`. Populated from
  overheard master beacons: relayed beacon's `hop_count + 1` = that
  neighbor's distance to master.
- Size: compile-time `LATTICE_NEIGHBOR_MAX` (default 8).
- Next hop = freshest neighbor with **min distance strictly less than own
  distance** (strict inequality prevents two equal-distance siblings
  ping-ponging a frame).
- Eviction: stale first (reuse `STALE_PEER_THRESHOLD_MS` = 8s semantics),
  then largest distance.
- `findNextHopToMaster()` reads the NeighborTable. The chosen neighbor is
  auto-added as an **unencrypted** ESP-NOW peer on first forward.
- Route repair is emergent: a dead relay stops beaconing, its entry goes
  stale, the next-best neighbor is selected. No new protocol messages.
- Data relay generalizes the existing enrollment relay logic
  (`Mesh.cpp:653-672`): bump hop_count, forward toward own next hop,
  ReplayCache dedup, `MAX_HOPS` bound.

## 4. Downlink — source routing, stateless relays

- Nodes send route reports (existing `OP_ROUTE_REPORT`) periodically **and
  on next-hop change**. The report's payload (opcode etc.) is AEAD-sealed
  like all payloads; the relay path accumulates in the plaintext
  `route_path` header field (relays append their MAC in flight — possible
  precisely because that field is mutable and excluded from AAD). The
  current in-payload path encoding (`[opcode][path_len][MACs…]`,
  `Mesh.cpp:806-840`) migrates to the header field.
- Consequence: the path a master learns is relay-asserted, not
  E2E-authenticated. A malicious relay could falsify it — yielding at worst
  misrouted downlinks (DoS-equivalent, same class as the accepted blackhole
  risk in Section 2). Uplink data integrity is unaffected.
- Master keeps a route table: node MAC → latest path. RAM-only, rebuilt
  from reports. Size: compile-time `LATTICE_ROUTE_TABLE_MAX` (default 32;
  raise for large deployments — master is hub-side with headroom).
- Downlink frames carry the reversed path in `route_path[]`. A relay finds
  its own MAC at index *i* and forwards to index *i+1*. Relays hold zero
  routing state — this is the property that scales.
- **Parse bounds:** `route_len ≤ MAX_HOPS`, all path indexing bounds-checked
  (same defect class as the OOB ack-buffer fix in PR #31; fuzz-tested).
- Stale route ⇒ downlink lost ⇒ next route report heals it. The JOIN_ACK
  broadcast workaround remains only as the enrollment-time fallback.

## 5. Dual-master data failover (#8)

- JOIN_ACK gains `secondary_master_mac(6) + secondary_public_key(32)`. The
  server (which manages both masters) supplies these via the primary.
- At enrollment the leaf derives a second `k_up`/`k_down` pair for the
  secondary and persists the secondary pubkey in EEPROM (secondary-MAC slot
  already exists at `EepromManager.h:497`; a pubkey slot is added).
- Failover behavior (TOFU-adopt secondary when primary goes silent) is
  already implemented and tested; after this change the post-failover
  uplink is encrypted with the secondary's keys and is decryptable.
- The secondary master obtains each leaf's pubkey via server sync over its
  serial link. **Confirmed assumption:** both masters are hub/server-connected.

## 6. Error handling and repair

- AEAD decrypt/tag failure → quiet drop + error counter (matches the
  finding-#9 pattern; no `err::fail` loops).
- No neighbor with smaller distance → quiet drop; wait for next beacon
  (existing behavior).
- seq wrap → epoch bump + persist (Section 2).
- Replay protection: existing ReplayCache unchanged; (origin, epoch, seq)
  remains the dedup key.

## 7. Testing

- Enable both `DISABLED_` e2e tests — they are the executable specs for
  #7 and #8.
- New e2e scenarios (PR #28 sim suite): 3-hop chain uplink; downlink source
  route delivery; route repair (kill relay mid-test); ciphertext tamper
  (bit-flip → drop); replay reinjection; equal-distance sibling ping-pong
  guard.
- Unit tests: AEAD known-answer vectors; nonce construction including seq
  wrap; NeighborTable eviction ordering; `route_path` parse bounds/fuzz.

## 8. Phasing (each phase = its own plan + PR)

1. **Protocol v3 + E2E AEAD**, direct-range only — crypto core, flag day.
2. **NeighborTable + multi-hop data uplink** — closes gap #7.
3. **Source-routed downlink** — master route table + path forwarding.
4. **Dual-master keying + data failover** — closes gap #8.

Phases 2–4 each end by enabling their corresponding e2e tests.
