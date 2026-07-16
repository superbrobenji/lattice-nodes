<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Design Gap: Multi-Hop Data Uplink

**Status:** Open — needs its own design/brainstorming session.
**Discovered:** 2026-07-15, while building the e2e mesh simulation suite (Task 9).
**Executable spec:** `tests/e2e/scenarios/test_multihop_e2e.cpp` →
`DISABLED_SensorOutOfMasterRangeRelaysThroughMiddleNode` (committed disabled;
its assertions are the intended behavior — drop the `DISABLED_` prefix once
this gap is closed).

## Summary

A sensor node that is **not** in direct RF range of the master can now *enroll*
through an intermediate relay node (fixed in Task 9b, commit `a35be64`), but it
**cannot send adapter data** (PIR motion, health reports, route reports) through
that same relay. The node has no usable route to the master at hop distance ≥ 2,
so every uplink attempt fails.

This is an architectural gap, not a localized bug: closing it requires
adjacent-hop key establishment and next-hop peer registration, which touch the
mesh's peering and encryption model. It was deliberately left unimplemented and
documented here rather than bolted on.

## Symptom

In a chain topology `leaf ── relay ── master` (leaf and master NOT linked):

1. `leaf` enrolls successfully through `relay` (enrollment requests and the
   JOIN_ACK are relayed multi-hop). `leaf` reaches `currentMaster.distance == 2`.
2. On its next periodic health report / any adapter-data transmit, `leaf` calls
   `Mesh::transmitCore`, which needs a next hop toward the master.
3. `findNextHopToMaster()` returns `nullptr`, and `transmitCore` raises
   `err::fail(COMM, MESH, 8, "MESH: message dropped, no route to master")`
   (`main/src/mesh/Mesh.cpp:333`).
4. Because the health-report timer fires periodically, this error repeats
   indefinitely. Data never reaches the hub.

## Root cause

Three mechanisms combine so that a distance-2 node has no route:

1. **Only the master is registered as a routable peer.** On enrollment a node
   registers the *master* (via JOIN_ACK → `registerPeerWithKey`), not the
   intermediate hop it actually heard the beacon/ACK through. Its `PeerRegistry`
   therefore contains the master's MAC but not the relay's.

2. **`PeerRegistry` deliberately refuses to auto-add unknown senders.**
   `main/src/mesh/PeerRegistry.cpp:67`: *"Enrollment is the only path for new
   peers — do not auto-add unknown senders here."* So the relay never becomes a
   peer of the leaf as a side effect of relaying.

3. **`findNextHopToMaster()` requires the next hop to be an in-range registered
   peer** (`main/src/mesh/Mesh.cpp`): it looks for a peer whose MAC equals
   `currentMaster.nextHop` AND `isPeerInRange`. For a distance-2 leaf the next
   hop is the relay, which is not a registered peer, so the lookup returns
   `nullptr`.

Underlying constraint: ESP-NOW links are encrypted with a **per-peer LMK derived
by Curve25519 ECDH** (`MeshCrypto.h`), and there is **no shared mesh-wide key**.
A node can only send an encrypted frame to a peer it has performed the ECDH
handshake with. It has done so with the master, not with the relay — so even if
the relay were added to the registry, there is no key material to encrypt the
hop to it.

## Side effect introduced by the Task 9b enrollment-relay fix

Before Task 9b, an out-of-range node simply never enrolled (its enrollment
request was dropped at the first non-master node). After Task 9b it enrolls and
then **error-loops on every uplink**. This is strictly more visible failure, not
a regression in delivered functionality (no data reached the hub in either
case), but it means: **do not rely on out-of-range enrollment in the field until
this gap is closed** — an enrolled-but-uplink-dead node is worse operationally
than an obviously-unenrolled one.

## What a solution must provide

- A way for a node to establish an encrypted link (or authenticated route) to
  its **next hop toward the master**, not only to the master itself.
- Next-hop peer registration that does not reopen the security hole closed in
  Task 7 (an unauthenticated JOIN_ACK must not be able to register/re-key an
  arbitrary peer — see `Mesh::registerPeerWithKey`'s `allowRekey`/first-contact
  gating).
- Consistency with the existing routing fields (`currentMaster.nextHop`,
  `hop_count`, `MAX_HOPS = 10`) and the relay-jitter / ReplayCache machinery
  already used for multi-hop enrollment and JOIN_ACK relay.

## Open design questions for the brainstorm

1. **Keying model.** Per-hop ECDH links (each node handshakes with each
   neighbor) vs. a shared mesh group key distributed at enrollment vs. a hybrid
   (group key for relay, per-peer for endpoints). Trade-offs: memory
   (`MAX_PEERS = 10`), rekey/rotation, blast radius of a compromised node.
2. **Neighbor discovery.** How does a node learn and authenticate its neighbors
   (the relay it can hear) — from beacons? A dedicated neighbor-announce? What
   authenticates a neighbor so this doesn't become a new spoofing surface?
3. **Route table.** Is `currentMaster.nextHop` (single next hop toward master)
   sufficient, or is a small forwarding table needed for >2 hops / redundant
   paths / dual-master?
4. **Downlink symmetry.** The same next-hop problem exists for server→node
   commands to a distance-2 node (JOIN_ACK already broadcasts as a workaround;
   see the amplification note in Task 9c). A routing solution should cover both
   directions.
5. **Failure/repair.** What happens when a relay goes offline — re-discovery,
   timeout, fallback to the secondary master (dual-master, Task 12)?

## Related gap: dual-master data failover

The same "a node is only keyed to the master it enrolled with" constraint blocks
dual-master **data** failover (Task 12). In dual-master mode a node correctly
TOFU-learns a secondary master's beacon and *adopts* it as `currentMaster` when
the primary goes silent (route-adoption failover works — see
`tests/e2e/scenarios/test_dual_master_e2e.cpp`
`LearnsSecondaryMasterInDualMode`, enabled). But the node enrolled with, and ran
ECDH with, only the **primary**, so it has no encrypted link to the secondary:
its first uplink after failover raises
`err::fail(COMM, MESH, 8, "no route to master")`. The full failover scenario is
committed as `DISABLED_FailsOverToSecondaryMasterWhenPrimaryGoesSilent`.

A solution must give a node key material for **every master it may fail over
to** — e.g. the server provisioning both masters' keys at enrollment, or a
shared group key. This is the same keying-model design question as above (Q1),
just applied to the master set rather than intermediate hops, and should be
solved together with it.

## Related work already landed

- Multi-hop **enrollment** relay: commit `a35be64`.
- Concurrent-enrollment queue: commit `2e3c1f5`.
- Enrollment/JOIN_ACK relay dedup + amplification bound: Task 9c.
- JOIN_ACK re-keying security gating (must not be weakened by any next-hop
  registration): commit `085ff85` (Task 7).
