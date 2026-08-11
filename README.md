# Lattice — ESP-NOW Mesh Network Firmware

[![CI](https://github.com/superbrobenji/lattice-nodes/actions/workflows/unit-tests.yml/badge.svg)](https://github.com/superbrobenji/lattice-nodes/actions/workflows/unit-tests.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Low-latency, encrypted, self-healing mesh network firmware for ESP32 devices, built on
**ESP-IDF** (no Arduino IDE/`arduino-cli` involved). Nodes talk peer-to-peer over ESP-NOW; a
master node bridges the mesh to a host server over USB serial. Sensor/control data is protected
end-to-end (node ↔ master) with X25519 + ChaCha20-Poly1305 AEAD, on top of enrollment
(TOFU master-pubkey pinning) and a pluggable adapter system for the actual sensor/IO hardware.

---

## Ecosystem

This repo is the **firmware** piece of a three-repo system:

- **`lattice-nodes`** (this repo) — the ESP32 firmware that runs on every mesh node.
- **[`lattice-hub`](https://github.com/superbrobenji/lattice-hub)** — the Go server the master
  node talks to over USB serial; handles enrollment approval, the REST API, and dashboards.
- **[`lattice-protocol`](https://github.com/superbrobenji/lattice-protocol)** — the shared
  wire-format source of truth. Vendored here as a git submodule at
  [`firmware/main/lib/lattice-protocol`](firmware/main/lib/lattice-protocol) (currently pinned to
  `v0.6.0`); `lattice-hub` imports the same schema as a Go module.

See each sibling repo's own README for details — this repo only describes the firmware.

---

## Features

- **ESP-NOW mesh** — sub-10ms latency, no Wi-Fi router required.
- **End-to-end payload encryption** — X25519 ECDH key agreement + HKDF-SHA256 + ChaCha20-Poly1305
  AEAD, genuinely end-to-end between an originating node and the master (relays forward sealed
  bytes opaquely and cannot decrypt them). This is separate from the mesh-wide 16-byte AES PMK
  ESP-NOW itself uses to encrypt all radio traffic link-layer-only — payload confidentiality comes
  from the E2E AEAD, not the PMK.
- **Enrollment protocol with TOFU master-pubkey pinning** — a new node broadcasts its X25519
  public key; the master relays it to the server, and the server-approved `JOIN_ACK` is checked
  against a build-time-pinned master public key plus trust-on-first-use MAC learning before a node
  will accept it.
- **Chain-MAC route-report authentication** — an HMAC-SHA256 chain authenticates the relay path
  recorded in route-report frames, defending against forged routing data.
- **Dual-master failover** — nodes can TOFU-learn a primary and secondary master MAC, so a mesh
  can tolerate one physical master going offline.
- **Replay protection** — per-origin high-water-mark (epoch, sequence-number) tracking rejects
  replayed frames.
- **Adapter system** — runtime-selectable hardware roles, persisted to NVS and changed without
  reflashing. **PIR** (motion sensor) and **Serial** (server bridge) are implemented today; the
  wire protocol also reserves `LED`/`RELAY` adapter types for future use, but no firmware class
  implements them yet.
- **Tiger Style engineering** — static allocation only after boot, watchdog-fed main loop,
  assertions/fatal-on-invariant-violation at the boundaries that matter.
- **Seven-segment error codes** — an optional TM1637 display shows a 3-digit `T-M-S` fault code;
  a red-LED blink-count pattern conveys a coarser version even without the display.
- **Low-power CPU scaling** — the master runs a dynamic 80–240MHz frequency range; leaf nodes are
  pinned at 80MHz; both enable light sleep when idle.

The mesh wire format (`mesh_message`, protocol v5) is a 200-byte packed struct with a 64-byte
opaque data payload — see [`docs/server_requirements.md`](docs/server_requirements.md) for the
full field-by-field schema; it isn't duplicated here.

---

## Architecture

```
lattice-nodes/
├── firmware/                       # ESP-IDF project — this repo's firmware
│   ├── CMakeLists.txt
│   ├── partitions.csv
│   ├── sdkconfig.defaults
│   └── main/
│       ├── project_config.h        # All compile-time constants (pins, keys, limits, tuning)
│       ├── config/                 # Per-deployment master-pubkey pin header (gitignored)
│       ├── lib/lattice-protocol/   # git submodule — shared wire-format headers (see Ecosystem)
│       └── src/
│           ├── mesh/               # ESP-NOW transport, routing, enrollment, E2E crypto, the Mesh orchestrator
│           ├── adapter/            # Adapter base class + PIR/Serial adapter implementations
│           ├── hardware/           # GPIO drivers: buttons, LEDs, seven-segment display
│           ├── persistence/eeprom/ # NVS-backed persistence, split by domain (identity, peers, security, ...)
│           ├── crypto/             # Single mbedtls wrapper — the only file that includes mbedtls headers
│           ├── network/            # Shared MAC-table skeleton + small networking utilities
│           ├── error/              # Digit-based error codes + TM1637/LED fault signaling
│           ├── logging/            # UART-backed leveled logger
│           └── app/                # Boot/button/display state machines driven from main.cpp
├── tests/                          # Host-native unit + e2e simulation suite (CMake/CTest)
├── tools/gen_master_pubkey_pin.py  # Generates the per-deployment master-pubkey pin header
└── docs/                           # Deeper reference docs (linked throughout this README)
```

See [`REFACTORING_GUIDE.md`](REFACTORING_GUIDE.md) for the full collaborator-by-collaborator
module map and design principles — this section is intentionally just an orientation, not a
duplicate.

---

## Requirements

| Requirement | Version |
|-------------|---------|
| Toolchain | **ESP-IDF v5.5.1** (pinned via `firmware/dependencies.lock`, auto-generated by the IDF Component Manager on first build — install this exact minor version) |
| Target chip | `esp32` (plain ESP32, Xtensa dual-core — set via `idf.py set-target esp32`) |
| Build system | ESP-IDF / CMake only — **no Arduino IDE, no `arduino-cli`** |

---

## Quick Start

```bash
git clone --recurse-submodules https://github.com/superbrobenji/lattice-nodes.git
cd lattice-nodes
# (if you cloned without --recurse-submodules: git submodule update --init --recursive)

source ~/esp/esp-idf/export.sh    # or wherever ESP-IDF v5.5.1 is installed
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash
```

The **first** `idf.py build` on a fresh clone will fail until you generate
`firmware/main/config/master_pubkey_pin.h` (a gitignored, per-deployment file that pins the hub's
master identity into the firmware) via `tools/gen_master_pubkey_pin.py` — this and every other
first-time-setup step (including a build-free walkthrough for readers without prior ESP32
experience) is covered in full in
[`docs/getting_started.md`](docs/getting_started.md).

---

## `project_config.h` Reference

Every constant a first-time user should review before their first flash
(`firmware/main/project_config.h`):

| Constant | Default | Notes |
|----------|---------|-------|
| `DEV_MODE` | `false` | `true` skips EEPROM writes, always uses `DEFAULT_ADAPTER`, and takes role from `DEFAULT_DEV_MASTER` instead of persisted state. Leave `false` for any real flash. |
| `DEFAULT_DEV_MASTER` | `true` | Only matters when `DEV_MODE=true`: boots as MASTER (`true`) or leaf NODE (`false`). |
| `DEFAULT_ADAPTER` | `SERIAL_ADAPTER` | Adapter instantiated on first boot / always in `DEV_MODE`. Must stay `SERIAL_ADAPTER` for any node connected to the hub over USB. |
| `MASTER_BEACON_INTERVAL_MS` | `3000` | How often the master broadcasts its presence beacon. |
| `STALE_MASTER_THRESHOLD_MS` | `9000` | How long a node waits without a beacon before clearing its route to the master. |
| `DUAL_MASTER_MODE` | `false` | Enables the two-physical-master failover mode. |
| `WIFI_CHANNEL` | `1` | ESP-NOW channel — **must match on every node.** |
| `DEFAULT_MESH_KEY` | *(placeholder 16 bytes)* | Shared AES-128 PMK ESP-NOW uses to encrypt radio traffic. **Change before deployment** and reflash every node with the same key: `python3 -c "import os; print([hex(b) for b in os.urandom(16)])"`. |
| `RED_LED_PIN` / `GREEN_LED_PIN` | `33` / `26` | Error/status LEDs. |
| `CONFIG_BUTTON_PIN` / `RESET_BUTTON_PIN` | `32` / `25` | Role-toggle and factory-reset buttons. |
| `SEVSEG_DATA_PIN` / `SEVSEG_CLK_PIN` | `23` / `22` | TM1637 seven-segment display DIO/CLK. |
| `ENABLE_SEVSEG_DISPLAY` | `true` | Set `false` if no display is wired up. |
| `DEFAULT_PEERS` | *(2 placeholder MACs)* | Initial ESP-NOW peer list written to EEPROM on first boot (non-dev-mode, only if EEPROM has none yet). Replace with your real device MACs before flashing. |
| `DEFAULT_LOG_LEVEL` | `LogLevel::LOG_NONE` | Verbosity of serial log output. **Must stay `LOG_NONE`** on any `SERIAL_ADAPTER` node talking to the hub — log text would corrupt the framed protocol on the shared UART. |
| `DEFAULT_TX_POWER_PRESET` | `TxPowerPreset::OUTDOOR` | Named RF power preset (`SHORT_RANGE`=2dBm, `INDOOR`=14dBm, `OUTDOOR`=20dBm), persisted to EEPROM. |
| `SIMULATE_MODE` | `0` | Enables serial-injected fake sensor events for hardware-free dev/testing. Never enable in production. |
| `MAX_HOPS` | `8` | Maximum relay hops across the mesh. |
| `HEALTH_REPORT_INTERVAL_MS` | `30000` | Periodic per-node health broadcast interval. |

A handful of additional "Tiger Style" bounded-resource tuning constants (key-cache/neighbor/replay
table sizes, etc.) live further down the same file — the defaults are reasonable for typical mesh
sizes and generally don't need to change for a first deployment.

---

## Buttons

| Button | Pin | Action |
|--------|-----|--------|
| Config | 32 | Hold **5 s** → toggle master/node role (production mode persists to EEPROM and reboots ~2 s later to apply it). |
| Reset | 25 | Hold **5 s** → arms an EEPROM wipe; hold again for **5 s**, starting within a **3 s** confirm window → wipes role/peers/identity/enrollment state and reboots as a blank node. |

---

## Seven-Segment Error Codes

An optional TM1637 display (see `ENABLE_SEVSEG_DISPLAY`) shows a 3-digit `TMS` decimal code
(`code = T*100 + M*10 + S`); see [`docs/error_codes.md`](docs/error_codes.md) for the full
call-site registry.

| Digit | Meaning |
|-------|---------|
| T | Error type: 1=Generic 2=Sensor 3=Comm 4=Memory 5=Hardware 6=Config 7=Crypto |
| M | Module: 1=Core 2=Adapter 3=Mesh 4=EEPROM 5=Hardware |
| S | Sub-code 0–9 |

---

## Server Integration

See [`docs/server_requirements.md`](docs/server_requirements.md) for the full serial wire
protocol, message/adapter/opcode tables, and the enrollment handshake contract.

---

## Development

### Running Unit Tests

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel 2
ctest --test-dir tests/build --output-on-failure --parallel 4 --label-exclude e2e
```

### End-to-End Simulation Suite

`tests/e2e/` runs the whole mesh on the host — multiple firmware nodes over a virtual ESP-NOW bus
with a scripted server on the master's serial port — so enrollment, relay, replay protection,
dual-master failover, adapter hotswap, PIR data flow, and serial framing can all be tested without
hardware. These tests carry the ctest label `e2e`:

```bash
cmake --build tests/build --target lattice_e2e --parallel 2
ctest --test-dir tests/build --label-regex e2e --output-on-failure
```

They run on every PR to `main` in their own **E2E Tests** GitHub Action (also available on demand
via `workflow_dispatch`); the unit-test job stays unit-only via `--label-exclude e2e`. A few
scenarios are committed disabled where they depend on unimplemented multi-hop data routing — see
[`docs/design-gaps/multihop-data-uplink.md`](docs/design-gaps/multihop-data-uplink.md).

### Adding a New Adapter

See [`docs/adapter_development_guide.md`](docs/adapter_development_guide.md).

### Changing Default Adapter

Edit `DEFAULT_ADAPTER` in `firmware/main/project_config.h`.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Summary:
- `clang-format --style=file` before every commit
- No heap allocation after boot
- All errors via `firmware/main/src/error/Error.h`'s digit-based `lattice::err::fail`/`fatal` API
- Unit tests for logic changes

---

## License

GPL v3 — see [`LICENSE`](LICENSE).

## Security

See [`SECURITY.md`](SECURITY.md) for the vulnerability reporting policy.
