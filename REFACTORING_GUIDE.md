# Architecture Guide

This document is the module map and design-principles reference for the Lattice firmware
(`firmware/main/src/`). It describes the current, post-refactor architecture — the codebase has
been fully decomposed from an earlier monolithic `Mesh.h`/`Mesh.cpp` and a singleton
`EepromManager` into the collaborator-based structure documented below. If you're adding a new
module, read "Adding a Module" at the end first.

Other docs in this repo go deeper on specific areas and link back here for the architecture
overview: `docs/adapter_development_guide.md` (writing a new `Adapter`), `docs/error_codes.md`
(the error-code registry).

## Design Principles

- **Tiger Style.** Static allocation only, watchdog-fed main loop, assertions/fatal-on-invariant-
  violation at the boundaries that matter (see `src/error/Error.h`), no dynamic containers grown
  at runtime.
- **Encapsulation yes, inheritance sparingly.** This is a deliberate, source-verifiable convention,
  not just a slogan:
  - The one place the firmware uses **polymorphic (virtual-dispatch) inheritance** is
    `adapter::Adapter` → `PirAdapter` / `SerialAdapter`: `Adapter` declares `init()`/`loop()` as
    pure virtual, and callers hold/invoke through an `Adapter*` (e.g. `main.cpp`'s
    `adapter->init()`/`adapter->loop()`). This is the genuine "I need runtime polymorphism" case.
  - Elsewhere, inheritance shows up in a narrower, **non-virtual** form used purely for field/
    method reuse: `hardware::GpioInput` → `Button`/`Pir` and `hardware::GpioOutput` → `Led` share
    pin-validation and an `_initialized` flag, but every call site invokes a method on the
    concrete type — never through a `GpioInput*`/`GpioOutput*` base pointer. `GpioInput.h`'s own
    header comment is explicit about why: "never dispatched through a base pointer, so the vtable
    slot buys nothing" — hence no `virtual`, no vtable. Structural code-sharing, not polymorphism.
  - Where even that would be overkill, the codebase uses **free functions over a common base
    class**. `network/mac_table.h`'s `namespace lattice::mac_table` provides `find(...)` and
    `evict_oldest_by_ts(...)` — a byte-offset/stride-based "MAC-keyed table" skeleton — shared by
    five otherwise-unrelated classes (`NeighborTable`, `RouteTable`, `E2EKeyStore`, `ReplayCache`,
    `PeerRegistry`) that would each otherwise reimplement the same linear-scan-by-MAC and
    evict-oldest-by-timestamp loop. The header's own comment explains the choice: this dedups the
    loop "WITHOUT templates (avoids a template instantiated per entry type bloating flash)" — and
    a shared base class was rejected for the same reason a template was: it would either force a
    common layout/vtable across five structurally-different POD entry types, or need one template
    instantiation per type anyway.
- **No heap after boot.** Every collaborator's containers (peer lists, tables, caches) are
  fixed-capacity and reserved at construction/`init()` time — see each collaborator's own note
  below for its capacity model (fixed vs. role-conditional).

## Module Map — `firmware/main/src/mesh/`

22 files, ~4,327 lines (excluding the vendored `serialization/` subdirectory), organized into
~16 distinct collaborator classes/namespaces. `Mesh.h`+`Mesh.cpp` together are 863 of those 4,327
lines (421 + 442, about 20%) — the other 80% (3,464 lines across the remaining 20 files) is the
extracted collaborators this section documents. `Mesh` is a genuinely thin orchestrator over these
collaborators, not just an assertion — see the dedicated section below for the method-by-method
evidence.

### Radio / transport layer

**`broadcast_mac.h`** (16 lines) — single canonical definition of the ESP-NOW broadcast MAC
(`constexpr uint8_t BROADCAST_MAC[6] = FF:FF:FF:FF:FF:FF`), replacing what used to be ~7 duplicated
inline copies. No logic. Used by `MeshTransport.cpp` (registers it as a peer) and `Mesh.cpp`
(`isBroadcastTarget` check).

**`MeshTransport.h` / `.cpp`** (166 + 204 = 370 lines) — class `MeshTransport`. Owns all ESP-NOW/
Wi-Fi radio I/O: bring-up, the RX ring buffer (ISR→task handoff), and every outbound send
primitive. Does *not* interpret what a received frame means — no message-type switch; `Mesh` owns
dispatch.
- `setup()` — raw ESP-IDF Wi-Fi bring-up (STA mode, channel set).
- `setupEspNow(meshKey, peers)` — ESP-NOW init, PMK set, registers broadcast peer + every known peer.
- `sendMessage(target, msg, deviceMac)` — unicast send (no-op if target is self).
- `broadcastToAllPeers(msg, peers, deviceMac)` — unicast to every registered peer except self.
- `static sendBroadcast(msg)` / `static registerPeerWithEspNow(mac)` — static so `Enrollment` and
  other collaborators can call them without holding a `Mesh*`.
- `drain(MessageHandler)` — drains the RX ring buffer, invoking `handler(srcMac, msg)` per entry.
- `setDrainNotifyHandle(TaskHandle_t)` — lets the ISR trampoline wake a dedicated drain task.

### Peer & identity management

**`PeerRegistry.h` / `.cpp`** (60 + 185 = 245 lines) — class `PeerRegistry` (+ structs `PeerInfo`,
`MasterInfo`). RAM list of enrolled peers (MAC + Curve25519 public key + last-seen) with EEPROM
persistence. Does not decide routing or crypto — a keyed store only.
- `find(mac)` (mutable/const), `append`, `remove`, `isPeerInRange`, `updateLastSeen`.
- `loadFromEEPROM()`/`saveToEEPROM()`, `addAndPersist`/`removeAndPersist`.
- `count()`/`at()`/`begin()`/`end()` (range-for support).
- Depended on by nearly every other collaborator in this list.

**`Enrollment.h` / `.cpp`** (93 + 210 = 303 lines) — class `Enrollment`. Owns this node's own
keypair, TOFU-learned master MAC(s) (primary + secondary, for dual-master failover), the enrolled
flag, and the JOIN_REQUEST/JOIN_ACK handshake logic including master-pubkey-pin verification.
Grants `friend class Mesh;` for legacy direct-field access, but exposes read accessors
(`hasKnownMaster()`/`knownMaster()`/`hasKnownSecondaryMaster()`/`knownSecondaryMaster()`)
specifically so other collaborators like `MasterBeacon`/`FrameAuthorizer` don't need `friend`
access too.
- `init()` — load/generate keypair + TOFU state from EEPROM.
- `isEnrolled()`, `getPublicKey()`/`getPrivateKey()`.
- `sendRequest(...)` (broadcast JOIN_REQUEST), `processRequest(msg)` (master-side relay queueing),
  `processJoinAck(msg, ...)` (node-side: verify pin + fingerprint + TOFU-learn + register master).
- `enrollPeer(mac, pubKey, ...)` (master-side register+encrypt a new peer).
- `learnMasterMac`/`learnSecondaryMasterMac`.
- Owns a `PendingRelayQueue` member; `setRelayFn`/`setPendingRelay`/`drainPendingRelay`.

**`PeerEnrollment.h` / `.cpp`** (29 + 103 = 132 lines) — free functions in `namespace
lattice::mesh` (deliberately **not** a class — bridging three collaborators without giving any one
of them an artificial dependency on the other two).
- `registerPeerWithKey(mac, publicKey32, allowRekey, peers, enrollment, dualMasterMode)` — add-or-
  rekey a peer (rekey only allowed for server-approved enrollment, never over-the-air JOIN_ACK).
- `addPeer(mac, peers)` — UI/app-triggered peer add.
- `dispatchJoinAck(msg, deviceMac, isMaster, enrollment, registerFn)` — outer relay-vs-process
  routing for an incoming JOIN_ACK.

**`PendingRelayQueue.h` / `.cpp`** (50 + 32 = 82 lines) — class `PendingRelayQueue`. Bounded
(capacity 4), heap-free FIFO of `(mac, pubKey)` pairs extracted from `Enrollment`, which needs to
buffer enrollment requests received during one radio-drain pass before relaying them (a prior
single-slot latch dropped concurrent enrollments). Pure data structure.
- `push(mac, pubKey)` (drops+logs if full), `drainTo(fn)`.

### Routing

**`NeighborTable.h`** (203 lines, header-only) — class `NeighborTable`. RAM-only table of
forwarding candidates toward the master, learned from overheard beacons (MAC + hop-distance +
freshness). Routing-only — never holds key material, never consulted for E2E crypto (a deliberate
trust split from `PeerRegistry`).
- `observe(mac, masterDistance, nowMs)`, `observeAndMinDistance(...)` (fused observe + min-
  distance scan).
- `selectNextHop(ownDistance, nowMs, outMac)` (freshest neighbor strictly closer to master).
- `minFreshDistance(nowMs)`, `contains(mac)`, `clear()`.

**`RouteTable.h`** (88 lines, header-only) — class `RouteTable`. Master-side-only RAM table
mapping a node's MAC → the relay path (origin→master order) learned from that node's most recent
route report; used for downlink source routing. `Mesh` allocates this only when `isMaster` (leaves
never pay its ~2.25KB), via `std::unique_ptr<RouteTable>` in `reevaluateRouteTable()`.
- `record(nodeMac, path, pathLen, nowMs)`, `lookup(nodeMac, pathOut, pathLenOut)`, `clear()`.

**`UplinkRouter.h` / `.cpp`** (28 + 66 = 94 lines) — class `UplinkRouter`. Owns
`findNextHopToMaster` — single-slot "one active uplink relay at a time" bookkeeping.
- `findNextHopToMaster(currentMaster, peers, neighbors, deviceMac, nowMs)` — returns the chosen
  next-hop `PeerInfo*` or `nullptr`; side effect: registers the new hop with ESP-NOW and evicts the
  prior forwarding peer if it changed (bounded, to prevent an attacker exhausting the ESP-NOW peer
  table via beacon-spoofed relay MACs).

**`DownlinkRouter.h` / `.cpp`** (108 + 119 = 227 lines) — class `DownlinkRouter`. Owns downlink
relay, auto-peer-registration for downlink forwarding (a bounded LRU distinct from
`UplinkRouter`'s single slot, since multiple concurrent downlink forwards can coexist), and the
**routing decision** (not the security decision — that's `FrameAuthorizer`) for
`Mesh::processAdapterData`'s downlink half. Crypto-free and read-only in `classify()`.
- `classify(msg, deviceMac, isMaster, addressedToSelf, isBroadcastTarget, addressedToMaster,
  nextHopMacOut) const` → `RouteDecision{NotRouted, RelayTowardMaster, ForwardOnRoute, Flood,
  DropHopLimitExceeded}` (kept distinct from `NotRouted` — one is an unconditional drop, the other
  falls through to the security gate; collapsing them would be a correctness bug).
- `relayDownlink(msg, peers, deviceMac, transport)` — flood-fallback broadcast relay.
- `registerDownlinkPeer(mac, peers, currentMaster)` — bounded LRU registration.

**`MasterBeacon.h` / `.cpp`** (76 + 179 = 255 lines) — class `MasterBeacon`. Owns master-role
beacon broadcast timing, master-timeout detection, and incoming-beacon processing: TOFU master-MAC
learning, dual-master failover adoption, master-MAC pin enforcement, distance/freshness tracking
via `NeighborTable`, and duplicate-beacon-relay suppression with jittered relay scheduling. Does
*not* build the beacon message itself — that's `MeshMessenger::buildMessage` — only times + sends
a pre-built one.
- `intervalElapsed()` — true at most once per `MASTER_BEACON_INTERVAL_MS`.
- `send(msg, transport)`; `checkTimeout(isMaster, currentMaster, lastSeenMasterMac)`.
- `process(msg, deviceMac, isMaster, dualMasterMode, enrollment, neighbors, currentMaster,
  txState, ...)` — the full incoming-beacon pipeline.

### Messaging / send pipeline

**`MeshMessenger.h` / `.cpp`** (130 + 303 = 433 lines) — class `MeshMessenger`, the largest single
collaborator by line count. "The single place how does this node send something lives" — outbound
message construction and dispatch: sequencing, E2E sealing, uplink routing, downlink source-
routing, enrollment ACK construction. Also defines the module-wide `PROTO_VERSION = 5` constant and
the shared free function `isSealedType(messageType)` (also used by `FrameAuthorizer`).
- `buildMessage(type, data, msgType, deviceMac, currentMaster, txState)` — constructs a fresh
  `mesh_message` with sequencing/target stamped.
- `transmitCore(...)` — core send path: E2E-seals self-originated sealed traffic, seeds route-
  report chain-MAC, routes via `UplinkRouter::findNextHopToMaster`, sends or drops-with-log.
- `transmitDispatch(...)` — shared body for `Mesh::transmit()`/`transmitSelfOriginated()`.
- `broadcastAdapterData(type, data, deliverLocally, ...)` — master-only-meaningful broadcast.
- `sendDownlinkToNode(destMac, type, data, ...)` — master-only: seals with the destination's
  k_down, source-routes via `RouteTable` lookup or falls back to flood.
- `enrollPeer(...)` (2-arg and 4-arg/secondary-master overloads) — register peer, broadcast JOIN_ACK.
- `relayEnrollmentUplink(msg, ...)` — relays a JOIN_REQUEST one hop toward the master.

**`RouteReportHandler.h` / `.cpp`** (40 + 164 = 204 lines) — class `RouteReportHandler`. Route-
report protocol handling — send and receive/verify/relay/record, including chain-MAC (HMAC chain)
verification defending against route-path forgery.
- `sendRouteReport(isMaster, uplinkRouter, currentMaster, peers, neighbors, enrollment, e2eKeys,
  deviceMac, txState, messenger, transport)` — non-master only, builds+sends via
  `messenger.transmitCore`.
- `processRouteReport(msg, isMaster, ...)` — master branch: E2E-open, opcode check, hop-count
  bound, chain-MAC reconstruction/verify, records into `RouteTable` on pass; relay branch:
  accumulates the plaintext `route_path`, extends the HMAC chain, forwards.

**`CompactMessage.h` / `.cpp`** (97 + 49 = 146 lines) — struct `CompactMessage` + free functions
`toCompact`/`toWire`. A smaller in-RAM representation of `mesh_message` (≤220 bytes,
`static_assert`-enforced). **Accuracy note: not currently wired into any active code path.**
`MeshTransport.h`'s own header comment states it "was evaluated for `recvQueue`'s element type but
is NOT used here" — every message type flowing through the RX ring buffer needs at least one field
the wire form has that would force `CompactMessage` back up to wire size anyway. It has a dedicated
unit test (`tests/unit/test_compact_message.cpp`) but no production call site. Treat this as a
reserved/tested-but-unused utility, not as something actively shrinking queue RAM today.

### Security / crypto

**`MeshCrypto.h`** (35 lines, header-only) — `namespace lattice::mesh::crypto`. Just the X25519
keypair-generation branch, wrapping `lattice::crypto::x25519_keygen` with the project's fatal-error
convention.
- `generateKeypair(priv32Out, pub32Out)`.

**`E2ECrypto.h`** (110 lines, header-only) — `namespace lattice::mesh::crypto`. The AEAD sealing
primitives for end-to-end payload encryption: ECDH shared-secret computation, HKDF direction-split
key derivation (k_up/k_down), nonce/AAD construction, seal/open. Pure crypto plumbing, no routing
awareness.
- `computeSharedSecret`, `deriveE2EKeys(ownPriv, peerPub, kUpOut, kDownOut)`, `buildNonce`,
  `buildAad`, `sealPayload(key32, msg)`, `openPayload(key32, msg)`.

**`E2EKeyStore.h`** (103 lines, header-only) — class `E2EKeyStore`. RAM-only cache of derived E2E
key pairs, one entry per peer MAC, to avoid a fresh X25519 exchange on every send. Round-robin
eviction when full. Capacity is role-conditional (larger on master, smaller on leaves) via
`setCapacity()`, called from `Mesh::reevaluateRouteTable()`.
- `setCapacity(maxEntries)` (idempotent no-op if unchanged, else reallocates and drops cache).
- `getKeys(mac, ownPriv32, peerPub32, kUpOut, kDownOut)` — returns cached or derives-and-caches.
- **Caveat:** pointers returned via `kUpOut`/`kDownOut` are invalidated by any subsequent
  `getKeys()` call that causes an eviction — callers must use immediately, never cache across calls.

**`E2EKeyLookup.h`** (40 lines, header-only) — free functions in `namespace lattice::mesh`.
Bridges `MasterInfo` + `PeerRegistry` + `Enrollment` + `E2EKeyStore` for the two common
"get me k_up/k_down for X" lookups, kept as free functions to avoid giving `E2EKeyStore` an
artificial dependency on `PeerRegistry`/`Enrollment`.
- `masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, kUpOut, kDownOut)` (leaf-side).
- `peerE2EKeys(originMac, peers, enrollment, e2eKeys, kUpOut, kDownOut)` (master-side).

**`RouteMac.h`** (74 lines, header-only) — `namespace lattice::mesh::routemac`. Route-report
chain-MAC (HMAC-SHA256, truncated to 8 bytes) authenticating the plaintext `route_path`/
`route_len` header fields a route report accumulates hop-by-hop, keyed off each hop's own pairwise
k_up with the master — defends against route-path forgery. A separate concern from `E2ECrypto.h`
(header-field authentication, not payload confidentiality).
- `buildHopContext(msg, prev_hop, this_hop, out_ctx)` (30-byte context).
- `chainStep(secret, hop_ctx, prev_mac, out_mac)`.

**`FrameAuthorizer.h` / `.cpp`** (30 + 105 = 135 lines) — class `FrameAuthorizer`. The security/
authorization half of what used to be `Mesh::processAdapterData` — the most security-sensitive
extraction in the refactor. Owns the master-not-self-addressed sealed-type rejection gate, both
directions' E2E-open, and the config-opcode (`OP_CONFIG_SET`/`OP_NODE_ID_SET`) authorization gates
that prevent a forged plaintext broadcast from triggering a state-changing reconfiguration. Does
*not* do routing (`DownlinkRouter`'s job) or local-delivery dispatch (`Mesh` keeps that).
- `authorize(msg, isMaster, addressedToSelf, currentMaster, peers, enrollment, e2eKeys,
  openedOut) -> AuthResult{Rejected, Authorized}` — `openedOut` written only on `Authorized`.
- Deliberately uses `Enrollment`'s public accessor API rather than requesting `friend` access.

**`ReplayCache.h`** (169 lines, header-only) — two structs: `ReplayCache` and
`OutboundSequenceState`. (1) `ReplayCache` — per-origin high-water-mark (epoch, seq) replay
detection for *incoming* frames, LRU-evicted by origin when full. (2) `OutboundSequenceState` —
this node's own outbound sequence counter, relay-dedup bookkeeping (used by `MasterBeacon` to
suppress duplicate beacon relays), the guarded sequence-draw handling the 0xFFFF→0 epoch-wrap case,
and a seal-time AEAD nonce-reuse guard (`checkEpochRollback`) that halts the node via `err::fail`
if a seal would ever reuse a nonce.
- `ReplayCache`: `init()`, `isReplay(msg, nowMs)`.
- `OutboundSequenceState`: `init(epoch)`, `nextSeq()`, `bumpEpoch(newEpoch)`,
  `markRelayed`/`wasRelayedBefore`, `nextSeqGuarded()`, `checkEpochRollback(epoch, seq)`.

### `Mesh.h` / `Mesh.cpp` — the orchestrator (421 + 442 = 863 lines)

`Mesh` owns 14 collaborator instances as members (`peers`, `transport`, `beacon`, `router`,
`uplinkRouter`, `messenger`, `routeReportHandler`, `frameAuthorizer`, `replay`, `txState`,
`enrollment`, `e2eKeys`, `neighbors`, `routes` (unique_ptr, master-only)) plus its own small
irreducible state (`deviceMacAddress`, `lastSeenMasterMac`, `meshKey`, `currentMaster`, `isMaster`,
`_dualMasterMode`, deferred-relay-jitter fields, and a handful of loop timers). Method by method:

| Method | ~Lines | What it does |
|---|---|---|
| `Mesh()` ctor | ~10 | Zero-init MAC/master-info fields, set singleton `instance`. |
| `readMacAddress()` | ~19 | Own logic — reads the radio MAC once at boot, caches it for other collaborators. |
| `init()` | ~46 | Sequences: `loadPersistentState()` → `reevaluateRouteTable()` → boot-epoch load/save/replay-init → `setupRadio()` → apply TX power preset from EEPROM → `transport.setupEspNow(...)`. Pure sequencing. |
| `setupRadio()` | ~10 | `transport.setup()`, then `readMacAddress()` + `peers.setDeviceMac(...)`. |
| `loadPersistentState()` | ~11 | `peers.loadFromEEPROM()`, `loadMeshKeyFromEEPROM()`, `enrollment.init()`. |
| `handleReceivedMessage(srcMac, msg)` | ~51 | Main dispatch: proto-version check + `replay.isReplay()` + `peers.updateLastSeen()`, then a `switch(msg.message_type)` into `enrollment.processRequest`/`messenger.relayEnrollmentUplink`/`processJoinAck`/`beacon.process`/`processAdapterData`/`routeReportHandler.processRouteReport` — one line each. |
| `transmit()` / `transmitSelfOriginated()` (static) | ~11 each | Trampolines through the `instance` singleton into `messenger.transmitDispatch(...)`. No decision logic. |
| `broadcastMasterBeacon()` | ~10 | `beacon.intervalElapsed()` guard → `messenger.buildMessage(...)` → set 2 fields → `beacon.send(...)`. |
| `loadMeshKeyFromEEPROM()` | ~29 | Own logic — EEPROM load, DEV_MODE override, unset-detection, default fallback + persist. No natural collaborator owns "mesh PMK provisioning." |
| `debugDumpRadio()` | ~20 | Own logic — dev-mode-gated diagnostic hex dump. |
| `checkMasterTimeout()` | ~3 | Pure forward to `beacon.checkTimeout(...)`. |
| `tickEnrollmentBroadcast(nowMs)` | ~11 | Own small state machine — 10s retry timer around `sendEnrollmentRequest()`. |
| `processAdapterData(msg)` | ~72 | Largest remaining logic: computes 3 booleans (`addressedToSelf`/`isBroadcastTarget`/`addressedToMaster`), calls `router.classify(...)`, switches on the 5-way `RouteDecision`, and on `NotRouted` calls `frameAuthorizer.authorize(...)` then dispatches to `externalRecvCallback` + conditionally `router.relayDownlink(...)`. Orchestration-with-glue — every check was moved verbatim from the collaborators it calls. |
| `processJoinAck(msg)` | ~4 | Pure forward to `lattice::mesh::dispatchJoinAck(...)`. |
| `loop()` | ~39 | Per-tick sequencing: `eeprom::flushIfDirty()` → `enrollment.drainPendingRelay()` → timer-gated `routeReportHandler.sendRouteReport(...)` → deferred jittered-beacon-relay dispatch → `isMaster`-gated `broadcastMasterBeacon()`. No business logic. |

`Mesh.h`'s own logic is almost entirely one-to-three-line forwards to collaborators
(`broadcastAdapterData`→`messenger.broadcastAdapterData`, `sendDownlinkToNode`→
`messenger.sendDownlinkToNode`, `addPeer`→`lattice::mesh::addPeer`, `removePeer`→
`peers.removeAndPersist`, `getPeerList`/`getPeerCount`→`peers.*`, `enrollPeer`→
`messenger.enrollPeer`, `drain()`/`setDrainNotifyHandle()`→`transport.*`, `getDevicePublicKey()`→
`enrollment.getPublicKey()`, `isEnrolled()`→`enrollment.isEnrolled()`). The one non-trivial header
method is `reevaluateRouteTable()` (~13 lines) — allocates/frees the master-only `RouteTable` and
resizes `E2EKeyStore`'s capacity based on `isMaster`.

**Net assessment:** `Mesh` retains (a) the dispatch switch for received-message types, (b) the
downlink-routing-decision switch in `processAdapterData` (which itself delegates classification to
`DownlinkRouter` and authorization to `FrameAuthorizer`, keeping only the glue), (c) per-tick
sequencing in `loop()`, (d) MAC/keypair/role bootstrap sequencing in `init()`, and (e) a handful of
genuinely orchestration-owned state (deferred-relay jitter timing, enrollment-retry timer, mesh-key
EEPROM provisioning) that doesn't cleanly belong to any single extracted collaborator. It does
**not** contain routing decisions, crypto, sequencing/replay logic, peer storage, beacon timing, or
the send-construction pipeline — all of that lives in the 21 other files documented above.

### `serialization/` subdirectory — vendored/generated code

Contains **vendored/generated code only**, not hand-maintained: `mesh.pb.c`/`mesh.pb.h` (explicitly
marked "Automatically generated nanopb header", generated by nanopb-0.4.9.1 from a `.proto`
definition not present in this directory), plus `nanopb/` — the vendored nanopb runtime itself.

**Accuracy note:** this generated Protobuf machinery is **separate from** the hand-written
`mesh_message` C struct (the wire format `mesh/` operates on for ESP-NOW radio frames, shared with
the server via the `lattice-protocol` submodule). No file under `firmware/main/src/mesh/*.{h,cpp}`
includes `serialization/mesh.pb.h` — the only consumers in the whole firmware tree are
`src/adapter/serial/SerialAdapter.h` and `SerialFraming.cpp`. This nanopb machinery serves the
serial/host-facing framing protocol (server ↔ master over USB), not the mesh radio protocol. See
`docs/server_requirements.md` for the serial wire schema.

## Module Map — `firmware/main/src/adapter/` + `firmware/main/src/hardware/`

Full depth on writing a new adapter lives in `docs/adapter_development_guide.md` — this is the
map-level summary.

**`Adapter.h` / `.cpp`** — abstract base class for all sensor/bridge adapters. Current
`adapter_types` enum (`Adapter.h`) is `UNKNOWN_ADAPTER=0, SERIAL_ADAPTER=1, PIR_ADAPTER=2` — no
`LED_ADAPTER` (removed; only the wire protocol's separate C header still reserves `LED`/`RELAY`
type IDs, unused by any firmware adapter class today). Base ctor is `explicit Adapter(uint8_t
pin)` — the adapter type is *not* a constructor parameter; each subclass sets `_adapterType` in its
own ctor body.
- `virtual bool init() = 0` / `virtual void loop() = 0` — pure virtual, implemented by subclasses.
- `void onMeshData(const mesh_message&)` — non-virtual shared entry point for inbound mesh data:
  runs control-opcode dispatch, then filters by `data_type==_adapterType` before calling the
  virtual `onMeshDataImpl` (except `SerialAdapter`, which receives every message unfiltered since
  it's the master's uplink).
- `void sendDataThroughMesh(adapter_types type, const uint8_t* data)` — invokes the installed
  `mesh_transmit_fn`, or `err::fail(CONFIG, ADAPTER, 1)` if none is set.
- Shared control-op dispatch table (`kControlOps`): `OP_CONFIG_SET`, `OP_NODE_ID_SET`,
  `OP_HEALTH_REQ`, `OP_TX_POWER_SET` handlers, reached via `dispatchControlOp(msg,
  rebroadcastOnMaster)` from both the mesh path and `SerialAdapter`'s direct-serial path.
- Health-report builder helpers: `buildHealthFrame(...)`, `sendSelfHealthReport()`,
  `healthTickDue(now)`, `resetHealthTick(now)`.

**`AdapterFactory.h` / `.cpp`** — all-static factory. `createAdapter(adapter_types, pin)` switches
on type (`PIR_ADAPTER`→`new PirAdapter(pin)`, `SERIAL_ADAPTER`→`new SerialAdapter(pin)`, default→
`err::fail(CONFIG, ADAPTER, 2)`); `createFromEEPROM()` reads the persisted type (unset/`0xFF`
defaults to `PIR_ADAPTER`). Owns the default-pin constants `PIR_ADAPTER_DEFAULT_PIN = 27` and
`SERIAL_ADAPTER_DEFAULT_PIN = 255` (sentinel, unused) — these live here, not in `project_config.h`.

**`adapter/pir/PirAdapter.{h,cpp}`** — drives a PIR motion sensor (owns a `hardware::Pir` member);
sends a `PIR_ADAPTER`-typed `ADAPTER_DATA` frame on motion plus periodic `OP_NODE_HEALTH` reports.
3-state machine (`IDLE`/`PENDING_SEND`/`COOLDOWN`), 3-second cooldown between triggers.

**`adapter/serial/SerialAdapter.{h,cpp}` + `SerialFraming.{h,cpp}`** — the master node's uplink
adapter: bridges `UART_NUM_0` (115200-8-N-1, shared with `Logger`) and the mesh. Forwards every
mesh-received message to the server; on the serial side, decodes length-prefixed nanopb frames
(`SerialFraming`, 2-byte LE length + payload, 256-byte max) and branches on message type
(`JOIN_ACK`, `ADAPTER_DATA`, `SERIAL_CMD_BROADCAST`) plus the shared control-opcode dispatch.

**`hardware/input/`** — `GpioInput` (shared pin-validation + `_initialized` base, non-virtual),
`Button` (debounced digital reader, active-HIGH, ~20ms debounce), `Pir` (edge-interrupt PIR driver
via `gpio_isr_handler_add`).

**`hardware/output/`** — `GpioOutput` (same pattern as `GpioInput`), `Led` (on/off/toggle plus a
non-blocking `pulse()`/`update()` blink state machine), `SevenSegDisplay` (bit-banged TM1637
4-digit display driver, does not inherit `GpioOutput` — only borrows its static pin-validation
helper).

## Module Map — `firmware/main/src/persistence/eeprom/`

`EepromManager` (singleton, deleted) is gone. The persistence layer is now 8 files under the flat
`lattice::eeprom` namespace, with **no facade/umbrella re-export header** — each consumer includes
only the specific domain header(s) it needs (e.g. `adapter/Adapter.cpp` includes only
`EepromIdentity.h` + `EepromDeviceConfig.h`).

| File | Responsibility | Key functions |
|---|---|---|
| `EepromCore.h/.cpp` | Lifecycle/foundation: NVS namespace init, dev-mode flag, wipe, shared `NVS_KEYS`/`EEPROM_SIZES` constants and the internal `core_internal::` KV-primitive layer every other domain `.cpp` calls into. | `init()`, `setDevMode`/`getDevMode`, `clearAll()` (factory reset), `flushIfDirty()`/`forceFlush()` (inline no-ops — NVS auto-commits per write). |
| `EepromIdentity.h/.cpp` | This node's X25519 keypair and assigned mesh node ID. | `loadKeypair`/`saveKeypair`, `loadNodeId`/`saveNodeId`. |
| `EepromRole.h/.cpp` | Master/dev role flags. | `loadMasterFlag`/`saveMasterFlag`, `loadDevFlag`/`saveDevFlag`. |
| `EepromSecurity.h/.cpp` | Mesh-wide security material and pinned master MACs. | `loadMeshKey`/`saveMeshKey`, `load/save/clearKnownMasterMac` (primary + secondary/failover variants). |
| `EepromPeers.h/.cpp` | Persisted peer registry records (up to `MAX_PEERS`=10). | `loadPeerList`/`savePeerList` (thin loops, kept for existing callers), plus per-record `loadPeerRecord`/`savePeerRecord`/`erasePeerRecord` so `PeerRegistry` can stream one record at a time. |
| `EepromDiagnostics.h/.cpp` | Boot-health/crash-loop bookkeeping. | `loadRebootCount`/`saveRebootCount`, `saveRebootReason`/`loadRebootReason`, `loadBootEpoch`/`saveBootEpoch`. |
| `EepromEnrollment.h/.cpp` (smallest file, 22 lines) | Single boolean: has this node completed enrollment. | `loadEnrolledFlag`/`saveEnrolledFlag`. |
| `EepromDeviceConfig.h/.cpp` | Device-level radio/adapter configuration. | `loadAdapterType`/`saveAdapterType`, `loadTxPowerPreset`/`saveTxPowerPreset`. |

All 8 `.cpp` files funnel NVS access through `EepromCore.h`'s `core_internal::` helpers: read
functions check `ensureInitialized()`, write functions additionally no-op under dev mode (dev mode
never persists to flash).

## Module Map — `crypto/`, `network/`, `error/`, `logging/`, `app/`

**`crypto/Crypto.h`** — single header, `namespace lattice::crypto`, wraps mbedtls
(`chachapoly`/`ecdh`/`ecp`/`hkdf`/`md`). Its own header comment states it's "the ONLY file that
includes mbedtls headers" — `E2ECrypto.h`, `MeshCrypto.h`, and `RouteMac.h` delegate here, so
swapping the crypto backend touches only this file. Public functions: `secure_zero`,
`x25519_keygen`, `x25519_shared`, `hkdf_sha256`, `hmac_sha256`, `aead_seal`, `aead_open` — all
stack-only, fixed-size buffers, mbedtls contexts freed on every code path.

**`network/`** — 4 small utility headers (no `MacAddress.h`; that class is deleted, along with all
references to it). `hw_mac.h` (`lattice::hw`) — cached "read this node's own station MAC" helper.
`mac_table.h` (`lattice::mac_table`) — the shared MAC-keyed-table skeleton described under Design
Principles above. `MacEq.h` (`lattice::mac`) — canonical 6-byte MAC-equality check. `mem.h`
(`lattice::mem`) — generic all-zero-buffer sentinel check. (Note: `broadcast_mac.h` lives in
`mesh/`, not here.)

**`error/`** — `Error.h` is the public API: `lattice::err::fail(ErrorTypeDigit, ModuleDigit,
uint8_t sub, const char* msg)` / `fatal(...)` (same signature; `fatal` halts forever after
signaling). The legacy two-argument `fail(utils::ErrorType, const char*)` overload is **gone** —
only a `toDigit(utils::ErrorType)` helper remains, used by the `check()`/`checkEsp()` convenience
wrappers, which always resolve to `ModuleDigit::CORE`, sub-code `0`. `ErrorCodes.h` defines the
digit enums (`ErrorTypeDigit`: `GENERIC=1 .. CRYPTO=7`; `ModuleDigit`: `CORE=1 .. HW=5`) and
`makeErrorCode(t,m,sub) = t*100 + m*10 + (sub % 10)`, a 3-digit `TMS` decimal code. `ErrorCore.h/
.cpp` (`err_core`) drives both the TM1637 numeric readout and a coarser error-LED blink-count
pattern from the same digit code. See `docs/error_codes.md` for the full call-site registry.

**`logging/Logger.h`/`.cpp`** — `class Logger` (`lattice::utils`), backed by native
`uart_write_bytes(UART_NUM_0, ...)` — not Arduino `Serial`. `LATTICE_LOG`/`LATTICE_LOGLN`/
`LATTICE_LOGF` macros gate at **compile time** on `LATTICE_DEFAULT_LOG_LEVEL`; at `LOG_NONE`
(the default) they fold to `((void)0)` and their arguments never reach `.rodata`. This matters on a
serial-attached master, where `Logger` and `SerialAdapter` share `UART_NUM_0` — text logging would
corrupt the binary framing if left enabled.

**`app/`** — three static, header-only coordination classes, each a thin state machine called from
`main.cpp`'s main loop: `BootManager::check()` (reset-reason logging, WDT crash-loop halt after 5
consecutive watchdog resets), `DisplayManager::tick()` (TM1637 steady-state readout: dashes while
unenrolled, else node ID with a decimal point if master), `ButtonHandler::tick()` (5-second-hold
config/reset button gestures — role toggle, and a two-stage arm-then-confirm EEPROM wipe).

## Adding a Module

1. Place it under the most relevant `src/<subsystem>/` directory, lowercase throughout, no
   per-adapter capitalized parent folder (e.g. `src/adapter/<name>/<Name>.{h,cpp}`, matching
   `src/adapter/pir/PirAdapter.{h,cpp}` — not `src/Adapter/<Name>_Adapter/`).
2. **Single Responsibility.** If a class touches two concerns, split it — this is the same
   discipline that turned one 1,382-line `Mesh` into the 16-collaborator breakdown above.
3. **Route all errors through `src/error/Error.h`'s digit-based API** —
   `lattice::err::fail(ErrorTypeDigit, ModuleDigit, uint8_t sub, const char* msg)` /
   `fatal(...)`. Do not add new call sites in the style of the old two-argument
   `fail(utils::ErrorType, const char*)` overload — it has been removed from the codebase.
4. **Use `hardware::GpioInput` / `hardware::GpioOutput`** as the base for any new single-pin
   hardware driver (non-virtual inheritance, for shared pin-validation/`_initialized` bookkeeping
   only — see the Design Principles note above on why these stay non-virtual).
5. **Reserve containers at construction/`init()` time; never grow one in `loop()`.** No heap
   allocation after boot.
6. **Dual CMakeLists registration.** Add new `.cpp` files to `firmware/main/CMakeLists.txt`'s
   component `SRCS` list so the real ESP-IDF build compiles them. If the file is hardware-
   independent (no direct mbedtls/ESP-IDF-driver calls), also add it to `tests/CMakeLists.txt`'s
   `FIRMWARE_SOURCES` list so the host unit-test binaries link it — and add any needed mocks/stubs
   for mbedtls-touching methods so those tests still build without real hardware.
