# Lattice — ESP-NOW Mesh Network Firmware

[![CI](https://github.com/superbrobenji/lattice-nodes/actions/workflows/unit-tests.yml/badge.svg)](https://github.com/superbrobenji/lattice-nodes/actions/workflows/unit-tests.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Low-latency, encrypted, self-healing mesh network firmware for ESP32 devices.
Nodes communicate peer-to-peer over ESP-NOW with AES encryption. A master node
bridges the mesh to a host server via USB serial using framed Protobuf messages.

---

## Features

- **ESP-NOW mesh** — sub-10ms latency, no Wi-Fi router required
- **AES-encrypted** — 16-byte mesh key stored in EEPROM
- **Enrollment protocol** — new nodes announce a Curve25519 public key;
  the server issues a JOIN_ACK before sensor data is forwarded
- **Replay protection** — per-boot epoch counter + sequence number
- **Adapter system** — runtime-switchable hardware roles (PIR sensor, serial
  bridge, LED, etc.) stored in EEPROM and changed without reflashing
- **Tiger Style engineering** — static allocation after `setup()`, WDT
  watchdog, exhaustive assertions, centralised error handling
- **Seven-segment error codes** — TM1637 display shows `T·M·S` fault codes
- **Low-power sensor nodes** — CPU scaled to 80 MHz; only master runs at 240 MHz

---

## Architecture

```
main/
├── main.ino                        # Setup / loop orchestration
├── project_config.h                # All compile-time constants (pins, keys, limits)
└── src/
    ├── Mesh/                       # Core mesh layer
    │   ├── Mesh.h / Mesh.cpp       # ESP-NOW init, receive queue, routing
    │   └── serialization/
    │       ├── mesh.pb.c / mesh.pb.h   # Nanopb-generated from mesh.proto
    │       └── nanopb/             # Nanopb runtime (pb_encode, pb_decode)
    ├── Adapter/                    # Adapter system
    │   ├── Adapter.h / Adapter.cpp # Abstract base; handles OP_CONFIG_SET for all types
    │   ├── AdapterFactory.h / .cpp # Creates/loads adapter from EEPROM
    │   ├── PIR_Adapter/            # Motion sensor
    │   └── Serial_Adapter/         # Serial bridge to host server
    ├── hardware/
    │   ├── input/                  # Button, Pir, GpioInput base
    │   └── output/                 # Led, SevenSegDisplay, GpioOutput base
    ├── core/
    │   └── Logger.h / Logger.cpp   # Levelled logging (LOG_NONE suppresses all output)
    ├── error/
    │   ├── Error.h                 # Public `lattice::err::fail / fatal` helpers
    │   ├── ErrorCore.h / .cpp      # Blink patterns + TM1637 error codes
    │   └── ErrorCodes.h            # Numeric error registry
    ├── persistence/
    │   └── EEPROM_Manager.h / .cpp # All EEPROM I/O; short-circuits in DEV_MODE
    └── network/
        └── MacAddress.h            # MAC address utilities
```

### Message struct (`mesh_message`, 75 bytes, packed)

| Field | Type | Notes |
|-------|------|-------|
| `protoVersion` | `uint8_t` | Always `1` |
| `messageType` | `MeshMessageType` | See enum below |
| `dataType` | `adapter_types` | `int32_t`, -1=UNKNOWN … 3=SERIAL |
| `originMacAddress` | `uint8_t[6]` | Set by sending device |
| `targetMacAddress` | `uint8_t[6]` | `FF:FF:FF:FF:FF:FF` = broadcast |
| `lastHopMacAddress` | `uint8_t[6]` | Updated at each relay hop |
| `data` | `uint8_t[12]` | Adapter payload / control opcodes |
| `hopCount` | `uint8_t` | Incremented per relay |
| `epochNum` | `uint32_t` | Boot count (replay protection) |
| `seqNum` | `uint16_t` | Per-boot counter (replay protection) |
| `enrollmentPublicKey` | `uint8_t[32]` | Curve25519 key; zero for non-enrollment |

Message types:

| Value | Name | Direction |
|-------|------|-----------|
| 0 | `MESH_TYPE_ADAPTER_DATA` | device ↔ device, device → server |
| 1 | `MESH_TYPE_MASTER_BEACON` | master → all |
| 2 | `MESH_TYPE_ENROLLMENT` | unenrolled node → master → server |
| 3 | `MESH_TYPE_SERIAL_CMD_BROADCAST` | server → master (broadcasts to all) |
| 4 | `MESH_TYPE_JOIN_ACK` | server → master → target node |

---

## Requirements

| Requirement | Version |
|-------------|---------|
| Hardware | ESP32-WROOM-DA (or compatible ESP32) |
| Arduino IDE | 1.8.x or later |
| ESP32 Arduino core | any recent release |
| arduino-cli (optional) | for command-line compile |

Dependencies bundled in the repo (no library manager needed):
- **nanopb 0.4.x** — `main/src/Mesh/serialization/nanopb/`

---

## Quick Start

### 1. Clone

```bash
git clone https://github.com/superbrobenji/lattice-nodes.git
cd Lattice-nodes
```

### 2. Configure

Edit `main/project_config.h`:

| Constant | What to change |
|----------|----------------|
| `DEFAULT_PEERS` | Your devices' MAC addresses (read from serial on first boot) |
| `DEFAULT_MESH_KEY` | Random 16-byte key, identical on every node |
| `DEFAULT_ADAPTER` | `PIR_ADAPTER` for sensors; `SERIAL_ADAPTER` for the master |
| `DEFAULT_LOG_LEVEL` | `LOG_NONE` when using `SERIAL_ADAPTER` (prevents text contaminating frames) |
| `WIFI_CHANNEL` | Must match on all nodes |

Generate a random mesh key:
```bash
python3 -c "import os; b=os.urandom(16); print(','.join(hex(x) for x in b))"
```

### 3. Compile and Upload

```bash
# Arduino IDE: open main/main.ino, select ESP32 WROOM-DA, upload

# Or via arduino-cli:
arduino-cli compile --fqbn esp32:esp32:esp32da main
arduino-cli upload  --fqbn esp32:esp32:esp32da -p /dev/ttyUSB0 main
```

### 4. First-boot Provisioning

On first boot, each unenrolled node prints its Curve25519 public key:
```
LATTICE_PUBKEY:3a7f2b...
```
The host server must read this, approve the node, and send a `MESH_TYPE_JOIN_ACK`
message (via the master's serial port) before the node begins forwarding sensor data.
See [`docs/server_requirements.md`](docs/server_requirements.md) for the full enrollment flow.

---

## project_config.h Reference

| Constant | Default | Notes |
|----------|---------|-------|
| `DEV_MODE` | `false` | Skip EEPROM writes; use `DEFAULT_ADAPTER`; never persist role |
| `DEFAULT_DEV_MASTER` | `true` | Role assumed in DEV_MODE |
| `DEFAULT_ADAPTER` | `SERIAL_ADAPTER` | Adapter used in DEV_MODE or on blank EEPROM |
| `MASTER_BEACON_INTERVAL_MS` | `3000` | How often master broadcasts |
| `STALE_MASTER_THRESHOLD_MS` | `9000` | Node clears master route after this silence |
| `WIFI_CHANNEL` | `1` | Must match on every node (1–13) |
| `DEFAULT_MESH_KEY` | *(example)* | **Change before deployment** |
| `DEFAULT_PEERS` | *(example MACs)* | **Replace with your device MACs** |
| `ENABLE_SEVSEG_DISPLAY` | `true` | Set `false` if no TM1637 display |
| `DEFAULT_LOG_LEVEL` | `LOG_NONE` | Set `LOG_DEBUG` for development |
| `MAX_HOPS` | `10` | Maximum relay hops |
| `HEALTH_REPORT_INTERVAL_MS` | `30000` | Periodic node health broadcast |

---

## Buttons

| Button | Pin | Action |
|--------|-----|--------|
| Config | 32 | Hold 5 s → toggle master/node role (saves to EEPROM + reboot in production) |
| Reset | 25 | Hold 5 s → arm EEPROM wipe; hold again within 3 s → wipe + reboot |

---

## Seven-Segment Error Codes

Display shows `T M S` (three hex digits). See [`docs/error_codes.md`](docs/error_codes.md) for the full registry.

| Digit | Meaning |
|-------|---------|
| T | Error type: 1=Generic 2=Sensor 3=Comm 4=Memory 5=Hardware 6=Config |
| M | Module: 1=Core 2=Adapter 3=Mesh 4=EEPROM 5=Hardware-Abstraction |
| S | Sub-code 0–F |

---

## Server Integration

See [`docs/server_requirements.md`](docs/server_requirements.md) for full wire protocol,
Protobuf schema, control opcodes, and enrollment flow.

---

## Development

### Running Unit Tests

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel
ctest --test-dir tests/build --output-on-failure
```

### Adding a New Adapter

See [`docs/adapter_development_guide.md`](docs/adapter_development_guide.md).

### Changing Default Adapter

Edit `DEFAULT_ADAPTER` in `main/project_config.h`.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Summary:
- `clang-format --style=file` before every commit
- No heap allocation after `setup()`
- All errors via `src/error/Error.h`
- Unit tests for logic changes

---

## License

GPL v3 — see [`LICENSE`](LICENSE).

## Security

See [`SECURITY.md`](SECURITY.md) for the vulnerability reporting policy.
