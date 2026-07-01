# Architecture Guide

This document describes the design principles and module responsibilities of
the Lattice firmware. It replaces an earlier draft that described planned
(but never implemented) refactor utilities.

## Design Principles

- **Tiger Style** — safety first, static allocation, assertions everywhere, WDT.
- **SOLID / DRY** — each class has one responsibility; no logic repeated across files.
- **No heap after setup()** — all buffers and peer lists are fixed-size arrays.

## Module Map

### `main/main.ino`
Orchestration only: initialises hardware in dependency order, wires callbacks,
runs the main loop. Does not contain business logic.

### `main/project_config.h`
Single source of truth for every compile-time constant. Edit this file only;
never scatter magic numbers in source files.

### `src/Mesh/Mesh.h — Mesh.cpp`
- Manages the ESP-NOW radio: WiFi init, peer registration, send/receive.
- Hosts a fixed-size lock-free receive queue (SPSC ring buffer) drained in `loop()`.
- Implements the enrollment protocol: Curve25519 keypair, `MESH_TYPE_ENROLLMENT`
  broadcast, `MESH_TYPE_JOIN_ACK` processing.
- Replay protection via per-boot epoch + sequence number.
- Beacon relay with jitter to prevent collision bursts.

### `src/Mesh/serialization/`
- `mesh.proto` → generated via nanopb → `mesh.pb.c / mesh.pb.h`.
- nanopb runtime (`pb_encode.c`, `pb_decode.c`) bundled here; no external library needed.
- Used by `Serial_Adapter` to encode/decode messages sent to/from the host server.

### `src/Adapter/`
- `Adapter` (abstract base): owns `mesh_transmit_fn`, handles `OP_CONFIG_SET` for
  **all** adapter types so any node can be reconfigured over the mesh.
- `AdapterFactory`: creates adapters from EEPROM type byte; provides default pins.
- `PIR_Adapter`: reads HC-SR501 motion events and broadcasts to master.
- `Serial_Adapter`: framed Protobuf I/O to host server; forwards mesh messages
  from server to mesh and mesh messages from nodes to server.

### `src/hardware/`
- `GpioOutput` / `GpioInput`: shared pin-validation and `_initialized` guard.
  All single-pin peripherals inherit from one of these.
- `Led`: blink patterns, error LED singleton.
- `SevenSegDisplay`: TM1637 driver; raises error codes on ACK failure.
- `Button`: debounced digital input.
- `Pir`: HC-SR501 interrupt-driven input.

### `src/core/Logger.h — Logger.cpp`
Levelled logging (`LOG_DEBUG` → `LOG_NONE`). Set `DEFAULT_LOG_LEVEL = LOG_NONE`
when the serial port is used for host-server framing — any text output corrupts frames.

### `src/error/`
- `Error.h`: public API — `lattice::err::fail()` / `lattice::err::fatal()`.
- `ErrorCore`: drives the error LED blink pattern and TM1637 display.
- `ErrorCodes.h`: numeric `T·M·S` code registry.

### `src/persistence/EEPROM_Manager.h — EEPROM_Manager.cpp`
All EEPROM reads and writes go through this singleton.
In `DEV_MODE` all writes are no-ops.
Centralises address constants (`EEPROM_ADDRESSES::*`).

### `src/network/MacAddress.h`
Utilities for MAC address comparison, formatting, and zero-checking.

## Adding a Module

1. Put it under the most relevant `src/<subsystem>/` directory.
2. Give it a single responsibility — if it touches two concerns, split it.
3. Route all errors through `src/error/Error.h`.
4. Use `GpioOutput` / `GpioInput` for new single-pin hardware drivers.
5. Reserve any dynamic containers at `setup()` time; never grow them in `loop()`.
