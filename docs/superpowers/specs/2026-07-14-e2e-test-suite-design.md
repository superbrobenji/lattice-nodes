# E2E Test Suite Design — Multi-Node Mesh Simulation on Host

**Date:** 2026-07-14
**Status:** Approved

## Goal

An end-to-end test suite that exercises all firmware functionality — enrollment,
multi-hop relay, route reporting, replay protection, dual-master failover,
adapter hotswap, PIR data flow, PIR health, and serial framing to the hub —
without physical hardware. Runs on host (macOS/Linux) and in CI.

Scope is firmware-only: the hub side is represented by a scripted `FakeHub`
inside the test process. The real Go orchestrator (lattice-hub) is tested in its
own repo; a full-stack smoke test may be added later as a separate phase.

## Approach

**In-process multi-node simulation with context-swap harness** (chosen over
one-process-per-node with a socket bus, and over refactoring the firmware to be
instance-based). Rationale: zero firmware changes, deterministic single-threaded
execution with virtual time, fast enough to run on every CI invocation, and it
builds directly on the existing host-test mock layer in `tests/mocks/`.

The firmware uses singletons (`Mesh::instance`, `EEPROM`, `Serial`, the ESP-NOW
mock's registered callback), so one process naturally holds one node's state.
The harness works around this by owning N per-node state snapshots and swapping
the relevant globals in and out around each node's tick.

## Architecture

```
tests/e2e/
├── CMakeLists.txt        # target lattice_e2e; reuses ../mocks, adds mbedtls
├── harness/
│   ├── SimClock.{h,cpp}      # owns _mockMillis; step(ms) advances the world
│   ├── NodeContext.{h,cpp}   # per-node snapshot of all mutable globals:
│   │                         #   EEPROM image, Serial rx/tx buffers,
│   │                         #   esp_now state (recv cb, peers, sent packets),
│   │                         #   WiFi MAC, Mesh::instance pointer
│   ├── SimNode.{h,cpp}       # one simulated node: real Mesh, real Enrollment,
│   │                         #   adapter built via real AdapterFactory,
│   │                         #   BootManager / DisplayManager / ButtonHandler.
│   │                         #   setup() and tick() mirror main.ino wiring
│   ├── VirtualBus.{h,cpp}    # topology graph (who hears whom); collects each
│   │                         #   node's captured esp_now sends and delivers them
│   │                         #   to reachable nodes' recv callbacks on the next
│   │                         #   step; supports link add/remove and frame drop
│   └── FakeHub.{h,cpp}       # scripted serial peer attached to the master
│                             #   node's Serial mock: decodes framed nanopb
│                             #   messages, performs the real Curve25519
│                             #   handshake, issues JOIN_ACK, can send commands
│                             #   (OP_CONFIG_SET etc.), records every received
│                             #   message for assertions
└── scenarios/                # gtest files, one per feature (see Test Plan)
```

### Scheduler loop

Per virtual millisecond step:

1. For each node (round-robin): swap its `NodeContext` globals in → run
   `tick()` (the node's loop body) → capture any esp_now sends and serial
   output → swap out.
2. `VirtualBus` routes captured frames to nodes reachable under the current
   topology; delivery happens at the start of the next step (one-step latency,
   deterministic ordering).
3. `FakeHub` consumes the master's serial output and queues any scripted
   responses into the master's serial input.

Single-threaded and fully deterministic: virtual clock, fixed node order,
no real I/O.

### Realism boundary

Everything below the `main.ino` glue layer is real firmware code compiled for
host: `Mesh.cpp`, `Enrollment.cpp` (with real mbedtls), `PeerRegistry.cpp`,
`ReplayCache.h`, adapters, `SerialFraming.cpp`, `EepromManager.cpp`, nanopb
serialization, and the `app/` headers (BootManager, DisplayManager,
ButtonHandler).

`main.ino` itself is not compiled: its file-scope globals (led/button/mesh/
adapter objects) cannot be instanced per node. `SimNode::setup()/tick()`
replicate its wiring (~300 lines). This is the one drift risk; it is accepted
and documented. If `main.ino` wiring changes, `SimNode` must follow.

### Simulated reboot

Destroy a `SimNode`, keep its EEPROM image, construct a new `SimNode` from the
same image. Exercises persistence (adapter type, master flag, mesh key) and the
epoch-bump path of replay protection.

## Unit Test Fix (same effort)

The existing unit tests do not test the real mesh code: `Mesh.cpp` and
`Enrollment.cpp` are excluded from `FIRMWARE_SOURCES` and
`tests/mocks/mesh_logic_impl.cpp` reimplements their methods (drift risk), with
crypto stubbed in `firmware_stubs.cpp`.

Changes:

- Add real `Mesh.cpp` and `Enrollment.cpp` to the unit build.
- Add mbedtls (host build, FetchContent, pinned tag + hash like googletest).
- Delete `tests/mocks/mesh_logic_impl.cpp`.
- Shrink `firmware_stubs.cpp` to genuinely hardware-only stubs.
- Re-point the 7 existing unit test files at the real code; update assertions
  where the reimplementation had drifted.

## Error Handling

- `err::fail` / `err::fatal` are hooked in the mock layer: any invocation fails
  the current test immediately, reporting node id and error code. A simulated
  node can never silently brick.
- `VirtualBus` asserts no frame is addressed to a MAC that no node owns
  (catches routing corruption).
- Watchdog analogue: a per-tick iteration budget; exceeding it fails the test
  (catches infinite loops that would trip the real WDT).

## Test Plan (scenarios)

| File | Covers |
|---|---|
| `test_enrollment_e2e` | Announce → relay → FakeHub JOIN_ACK → enrolled → data forwarded. Unenrolled node's data is blocked. |
| `test_pir_dataflow_e2e` | PIR trigger → mesh → master → framed protobuf at FakeHub with correct payload, origin MAC, dataType. |
| `test_multihop_e2e` | Topology A–B–master with A out of master range: relay works, hopCount increments, lastHopMacAddress updates, no loops or duplicate delivery. |
| `test_route_report_e2e` | MESH_TYPE_ROUTE_REPORT hop chain matches the actual path taken. |
| `test_replay_e2e` | Re-injected captured frame dropped; node reboot bumps epoch and is accepted; stale seqNum dropped. |
| `test_dual_master_e2e` | Two masters; primary goes silent past STALE_MASTER_THRESHOLD_MS; nodes fail over to secondary. |
| `test_hotswap_e2e` | FakeHub sends OP_CONFIG_SET; node swaps adapter type at runtime; persists to EEPROM; survives simulated reboot. |
| `test_pir_health_e2e` | PIR health opcodes flow end-to-end to FakeHub. |
| `test_serial_robustness_e2e` | Garbage bytes before/inside serial frames: framing recovers, no crash, subsequent frames decode. |

Assertions run against FakeHub's recorded messages, VirtualBus frame logs, and
(via `UNIT_TEST` access) node-internal state where needed.

## Build & CI

- New CMake target `lattice_e2e` under `tests/e2e/`, sharing the mock include
  path and firmware source list with the unit build (factored into a common
  CMake include).
- mbedtls via FetchContent, pinned tag with URL hash.
- **CI: manual trigger for now** — a separate workflow (or job) with
  `workflow_dispatch` only. Move to per-PR later once runtime and stability are
  proven. The unit-test workflow keeps running on PR as today, including the
  real-code unit fix.

## Out of Scope

- Full-stack test against the real lattice-hub orchestrator (possible later
  phase: firmware sim process + pty serial + Docker hub).
- Real concurrency/timing races (sim is single-threaded by design).
- RF-level effects beyond reachability and frame drop (no RSSI modelling).
