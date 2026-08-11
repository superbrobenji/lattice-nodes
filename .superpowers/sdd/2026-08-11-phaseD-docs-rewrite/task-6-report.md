# Task 6 Report — `docs/server_requirements.md` rewrite

**Status:** DONE

**Commit:** `bad9279` on branch `phaseD-task6-manual` ("docs(phaseD): rewrite server_requirements.md
with the verified current wire protocol")

## What was done

Full rewrite of `docs/server_requirements.md` per the brief's 10-section outline, primary source
`phaseD-research-wire-protocol.md` (read in full — all 9 sections plus the §0 corrections preamble).

1. **Ecosystem context** — brief paragraph (lattice-hub = reference server, lattice-protocol =
   shared schema source, both pinned to v0.6.0), no architecture deep-dive.
2. **Topology overview** — master↔server serial (115200 8N1), server never speaks ESP-NOW, plus a
   Mermaid topology diagram.
3. **§2 "CRITICAL: two wire schemas"** — led with this, framed as the doc's single most important
   correction: the server sees `mesh_MeshMessage` (nanopb, `mesh.pb.h`), not the raw 200-byte RF
   `mesh_message` struct. Explained why the two schemas differ and enumerated the specific fields
   that exist in one but not the other or exist-but-are-dead.
4. **Serial framing** — 2-byte LE length prefix, 256-byte max, plus the `encode()`/`decode()`
   asymmetry (which fields the master trusts vs. overwrites, by message type) and a practical
   "minimum fields the server needs to populate" summary per message type.
5. **Wire schema** — reconstructed the nanopb schema as a `.proto`-style block (all 16 tags) plus a
   field-by-field table, explicitly marking `routeLen`/`routePath`/`authTag` as dead fields and
   flagging the stale 60-byte `routePath` capacity as a hand-maintenance artifact.
6. **Message types** — the 6-row table with corrected directions (`MASTER_BEACON` RF-only;
   `JOIN_ACK`/`SERIAL_CMD_BROADCAST` serial-only, master rebuilds/translates rather than relaying
   verbatim).
7. **Adapter types** — the 5-row table, with an explicit "practical consequence" paragraph on what
   happens if the server sets a node to type 3/4 today (accepts + persists + reboots + fails to
   construct an adapter).
8. **Control opcodes** — the full 12-row table including all caveats (`OP_LED_*`'s
   inconsistent/unverified payload shape and `SIMULATE_MODE` byte collisions, `OP_ROUTE_REPORT`'s
   empty payload, `OP_COMMAND_ACK` never emitted, `OP_NODE_HEALTH`'s data_type vs. payload-byte-1
   mismatch), plus the `SERIAL_CMD_BROADCAST` routing-semantics pseudocode block and the
   `data[1..6]`-not-`targetMacAddress` targeting note for `CONFIG_SET`/`NODE_ID_SET`.
9. **Enrollment/JOIN_ACK flow** — all 4 parts (8a–8d) with the exact field-by-field table for what
   the server must send, and explicit framing that the master's RF-side `enrollPeer` code (which a
   server implementer might stumble on while reading firmware source) describes a *different* frame
   than what the server itself sends.
10. **What the server does NOT implement** — zero crypto (E2E AEAD, PMK aside) and no route-report
    validation, each with the "why" (master unseals before serial forward; route fields never reach
    the wire).
11. **Dual-master** — two independent physical masters, server-owned identity tracking, JOIN_ACK
    secondary-master fields, and the known cross-master pubkey-sync gap with its
    `docs/design-gaps/multihop-data-uplink.md` citation.

## Verification performed (self-check step from the brief)

Per the brief's explicit instruction, cross-referenced every opcode/message-type/adapter-type value
against the actual generated headers directly (not just the research file) as an independent final
pass:

- `firmware/main/lib/lattice-protocol/c/opcodes.h`, `message_types.h`, `adapter_types.h`,
  `mesh_message.h` — read in full; every value in the doc's tables (message types 0–5, adapter
  types 0–4, all 12 opcodes 0xB0–0xE0) matches these headers exactly.
- `firmware/main/src/mesh/serialization/mesh.pb.h` — read in full; confirmed all 16 nanopb tags,
  field types, and the stale 60-byte `routePath` capacity (vs. the RF struct's 48-byte capacity)
  cited in the doc.
- `firmware/main/lib/lattice-protocol/proto/mesh.proto` — read; confirmed it has tags through 14
  plus 17=`authPath` and no `secondaryMasterMac`/`secondaryPublicKey`, supporting the "two schemas
  are not the same" claim.
- `firmware/main/src/adapter/serial/SerialFraming.cpp`, `SerialAdapter.cpp` — read in full; verified
  the `encode()`/`decode()` field-population asymmetry, the `JOIN_ACK` handler's exact
  approve/reject logic, and the `SERIAL_CMD_BROADCAST` routing pseudocode against the actual code.
- `firmware/main/src/adapter/Adapter.cpp` — read in full; verified `kControlOps[]` (exactly 4
  entries), `opConfigSet`/`opNodeIdSet`'s `data[1]`/`data[7]` field offsets, and
  `isTargetedAtSelf`'s broadcast/unicast-match logic.
- `firmware/main/src/mesh/Enrollment.cpp`, `MeshMessenger.cpp` — read in full; verified
  `processJoinAck`'s exact 5-step order (fingerprint → pubkey pin → TOFU origin gate → peer
  registration → dual-master secondary read) and `enrollPeer`'s frame-construction fields
  (`data[0..4]`/`data[4..10]`/`data[10..42]` layout) match the doc's §8c/§8d text exactly.
- `firmware/main/src/mesh/RouteReportHandler.cpp` — read in full; confirmed `sendRouteReport()`
  always writes `data[1]=0`, supporting the `OP_ROUTE_REPORT` caveat.
- `firmware/main/src/adapter/AdapterFactory.cpp` — read in full; confirmed the `default:` case
  calls `err::fail(...)` and returns `nullptr` for unrecognized adapter types, and
  `adapterTypeFromEEPROM(0xFF) → PIR_ADAPTER`.
- `tests/e2e/harness/FakeHub.cpp`/`.h` — read in full; the reference test double's
  `approveEnrollment()`/`sendConfigSet()` match the doc's JOIN_ACK field table and
  `data[1..6]`-targeting claim exactly, including the comment confirming `data[0..3]` fingerprint
  is not checked by the master's serial handler (only by the enrolling node, on the master's
  *rebuilt* frame).
- `ls firmware/main/src/adapter/` — confirmed only `pir/`/`serial/` subdirectories exist, supporting
  the "no LedAdapter/RelayAdapter class anywhere" claim.

One environment note: this worktree's `lattice-protocol` git submodule was not checked out
initially (empty directory, `git submodule status` showed a `-` prefix). I ran
`git submodule update --init --recursive` in this worktree to populate it before reading the
generated headers directly — confirmed it checked out to the same pinned commit already recorded in
`.gitmodules`/the superproject index (`99cd30ccf4a5803c46810ce54563e1e6ff14f596`, tag `v0.6.0`), so
this was a read-only population step, not a state change to commit.

## Self-assessment

Factual accuracy: every field name, tag number, opcode value, and message-type/adapter-type value
in the doc was independently verified against the actual generated headers and firmware source in
this session (not transcribed from the research file on trust) — the research file's claims held up
without exception against direct source inspection. I also caught and fixed one small omission of
my own during a self-review pass: an early draft of §8a's enrollment-relay field list dropped
`protoVersion` (which `SerialAdapter::relayEnrollmentToServer` does set to `PROTO_VERSION`, not
zero) — corrected before commit.

## Concerns

None blocking, but flagging two things for the controller/reader rather than presenting them with
false confidence:

- The §3 claim that in-practice encoded frame sizes stay well under the 256-byte `MAX_PAYLOAD`
  limit is based on reasoning about which optional fields are ever set together (never
  `routeLen`/`routePath`/`authTag`; `public_key`/`secondaryMasterMac`/`secondaryPublicKey` only
  together on `JOIN_ACK`), not on an actual byte-counted encode of a worst-case `JOIN_ACK` frame. I
  did the arithmetic by hand (~180 bytes) and it comfortably clears 256, but I did not compile and
  run an actual encode to get an exact number, so I kept the doc's wording qualitative ("comfortably
  fits") rather than asserting a specific byte count I have not directly measured.
- The `OP_LED_*`/`OP_RELAY_SET` payload-shape/targeting inconsistency (documented no-MAC layout vs.
  the transport layer's unconditional `data[1..6]`-as-destination read) is real and confirmed in
  code, but since no LED/RELAY adapter exists to actually exercise that path, the doc's prediction
  of what a future unicast LED/relay command would need is inference from how the routing layer
  works today, not a tested fact — I labeled it explicitly as unresolved/unbuilt/untested rather
  than a confirmed contract, per the brief's own instruction not to assert the opcode spec's
  doc-comment as gospel.
