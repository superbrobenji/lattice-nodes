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
Wiring only: initialises hardware in dependency order, wires callbacks, runs main loop.
Delegates all logic to `src/app/` components.

### `main/project_config.h`
Single source of truth for every compile-time constant. Edit here only.

### `src/app/`
Application-level coordination extracted from `main.ino`. Header-only — no `.cpp` files.
- `BootManager` — reset reason check, WDT loop detection, reboot counter.
- `DisplayManager` — 7-seg enrolled/unenrolled/master state machine.
- `ButtonHandler` — config role-toggle and double-confirm EEPROM-wipe state machines.

### `src/mesh/`
- `Mesh` — thin orchestrator: ESP-NOW radio init, lock-free SPSC recv queue, routing dispatch, beacon relay, `loop()`.
- `PeerRegistry` — peer list CRUD, EEPROM serialisation, staleness checks.
- `Enrollment` — Curve25519 keypair load/generate, enrollment broadcast, JOIN_ACK processing, TOFU master MAC persistence.
- `ReplayCache` — per-boot epoch + sequence-number replay protection (header-only struct).
- `MeshCrypto` — ECDH shared-secret derivation, peer LMK, keypair generation, ESP-NOW peer registration (header-only inline functions).
- `serialization/` — nanopb-generated `mesh.pb.c/.h` + nanopb runtime. Do not hand-edit.

### `src/adapter/`
- `Adapter` — abstract base: owns `mesh_transmit_fn`, handles `OP_CONFIG_SET` for all adapter types.
- `AdapterFactory` — creates adapters from EEPROM type byte; provides default pins.
- `pir/PirAdapter` — HC-SR501 motion events → mesh broadcast.
- `serial/SerialAdapter` — framed Protobuf I/O to host server; health reports.
- `serial/SerialFraming` — 2-byte-length-prefixed protobuf encode/decode + byte-feed state machine. Testable without hardware.

### `src/hardware/`
- `GpioOutput` / `GpioInput` — pin-validation and `_initialized` guard. All single-pin peripherals inherit from one.
- `Led`, `SevenSegDisplay`, `Button`, `Pir` — single-pin peripheral drivers.

### `src/logging/`
Levelled logging (`LOG_DEBUG` → `LOG_NONE`). Set `DEFAULT_LOG_LEVEL = LOG_NONE` when the serial port is used for host-server framing.

### `src/error/`
- `Error.h` — public API: `lattice::err::fail()` / `lattice::err::fatal()`.
- `ErrorCore` — error LED blink pattern and TM1637 display.
- `ErrorCodes.h` — numeric `T·M·S` code registry.

### `src/persistence/`
`EepromManager` — all EEPROM reads/writes through singleton. `DEV_MODE` no-ops all writes. Centralises address constants in `EEPROM_ADDRESSES::*`.

### `src/network/`
`MacAddress` — MAC comparison, formatting, zero-checking utilities.

## Adding a Module

1. Place it under the most relevant `src/<subsystem>/` directory.
2. Single responsibility — if it touches two concerns, split it.
3. Route all errors through `src/error/Error.h`.
4. Use `GpioOutput` / `GpioInput` for new single-pin hardware drivers.
5. Reserve dynamic containers at `setup()` time; never grow them in `loop()`.
6. Add new `.cpp` files to `tests/CMakeLists.txt` `FIRMWARE_SOURCES` if they are hardware-independent (no mbedtls). Add stubs to `tests/mocks/firmware_stubs.cpp` for any mbedtls-using methods.
