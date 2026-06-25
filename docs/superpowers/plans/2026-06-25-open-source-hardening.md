# Open Source Hardening & Documentation Refresh

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare the Planetopia-nodes repo for public open-source release by fixing all documentation, removing private data, adding missing community/legal files, and closing the gap between CI claims and CI reality.

**Architecture:** No source-code changes. Work touches config files, documentation, CI workflows, and community health files. Each task is independent and produces a reviewable diff on its own.

**Tech Stack:** Markdown, YAML (GitHub Actions), `.clang-format` (LLVM syntax), GPL v3 License.

## Global Constraints

- All MAC addresses in the repo must be clearly fictitious examples (use `AA:BB:CC:DD:EE:FF` style, not real OUI prefixes).
- Never commit real credentials, keys, or device identifiers.
- Docs must match the *actual* code on disk, not planned-but-unimplemented classes.
- Keep every existing `docs/` file path stable — external references to them must not break.
- Commit message style: imperative present-tense, ≤72 chars first line.

---

## File Map

| File | Action | Reason |
|------|--------|--------|
| `LICENSE` | **Create** | Legally required to open-source |
| `SECURITY.md` | **Create** | Standard OSS responsible-disclosure |
| `CODE_OF_CONDUCT.md` | **Create** | Standard Contributor Covenant |
| `.github/ISSUE_TEMPLATE/bug_report.md` | **Create** | Community issue quality |
| `.github/ISSUE_TEMPLATE/feature_request.md` | **Create** | Community issue quality |
| `.github/pull_request_template.md` | **Create** | PR quality gate |
| `.clang-format` | **Create** | Referenced by CONTRIBUTING.md; was missing |
| `.github/workflows/unit-tests.yml` | **Modify** | Add clang-format + cppcheck jobs |
| `main/project_config.h` | **Modify** | Replace real device MACs with placeholder examples |
| `README.md` | **Rewrite** | Fix duplicate section, wrong file paths, missing enrollment/nanopb info |
| `REFACTORING_GUIDE.md` | **Rewrite** | Documents non-existent `src/utils/` classes; replace with accurate architecture guide |
| `docs/server_requirements.md` | **Modify** | Add enrollment protocol (JOIN_ACK, public key provisioning) |
| `docs/adapter_development_guide.md` | **Modify** | Fix `src/utils/Logger.h` → `src/core/Logger.h`; remove raw EEPROM.write patterns |
| `CONTRIBUTING.md` | **Modify** | Align CI claims with actual workflow |

---

## Task 1: LICENSE

**Files:**
- Create: `LICENSE`

**Interfaces:**
- Produces: `LICENSE` at repo root, GPL v3 full text.

- [ ] **Step 1: Download GPL v3 full text**

```bash
curl -o LICENSE https://www.gnu.org/licenses/gpl-3.0.txt
```

Verify the first line reads `GNU GENERAL PUBLIC LICENSE`:
```bash
head -1 LICENSE
```
Expected: `                    GNU GENERAL PUBLIC LICENSE`

- [ ] **Step 2: Verify file size is reasonable**

```bash
wc -l LICENSE
```
Expected: approximately `674` lines.

- [ ] **Step 3: Commit**

```bash
git add LICENSE
git commit -m "chore: add GPL v3 license"
```

---

## Task 2: Sanitize project_config.h (remove real device MACs)

**Files:**
- Modify: `main/project_config.h:65-68`

**Interfaces:**
- Consumes: nothing
- Produces: `DEFAULT_PEERS` containing clearly fictitious MAC addresses

The two hardcoded MACs (`EC:64:C9:5D:AC:18` and `EC:64:C9:5D:22:20`) are real device identifiers. Replace with obviously fictional examples and add a comment directing users to substitute their own.

- [ ] **Step 1: Replace the DEFAULT_PEERS block**

In `main/project_config.h`, find:
```cpp
inline constexpr uint8_t DEFAULT_PEERS[][6] = {
  {0xEC,0x64,0xC9,0x5D,0xAC,0x18}, // master
  {0xEC,0x64,0xC9,0x5D,0x22,0x20}  // sample node
};
```

Replace with:
```cpp
// TODO: Replace these with your actual device MAC addresses before flashing.
// Run `esptool.py chip_id` or read from the serial output on first boot.
// All nodes in a mesh must share identical WIFI_CHANNEL and DEFAULT_MESH_KEY.
inline constexpr uint8_t DEFAULT_PEERS[][6] = {
  {0xAA,0xBB,0xCC,0xDD,0xEE,0x01}, // master — replace with real MAC
  {0xAA,0xBB,0xCC,0xDD,0xEE,0x02}  // node   — replace with real MAC
};
```

- [ ] **Step 2: Add a warning comment to DEFAULT_MESH_KEY**

In `main/project_config.h`, find:
```cpp
// Global 16-byte AES key – ALWAYS used for ESP-NOW encryption
inline constexpr uint8_t DEFAULT_MESH_KEY[16] = {
  0x12,0x34,0x56,0x78, 0x9A,0xBC,0xDE,0xF0,
  0x11,0x22,0x33,0x44, 0x55,0x66,0x77,0x88};
```

Replace with:
```cpp
// Global 16-byte AES key – ALWAYS used for ESP-NOW encryption.
// WARNING: Change this before deployment. Every node in a mesh must share the same key.
// Generate a random key: python3 -c "import os; print([hex(b) for b in os.urandom(16)])"
inline constexpr uint8_t DEFAULT_MESH_KEY[16] = {
  0x12,0x34,0x56,0x78, 0x9A,0xBC,0xDE,0xF0,
  0x11,0x22,0x33,0x44, 0x55,0x66,0x77,0x88};
```

- [ ] **Step 3: Verify no real OUI prefix remains**

Run:
```bash
grep -n "0xEC,0x64,0xC9" main/project_config.h
```
Expected output: (empty — no matches)

- [ ] **Step 4: Commit**

```bash
git add main/project_config.h
git commit -m "chore: replace real device MACs with example placeholders"
```

---

## Task 3: Add .clang-format

**Files:**
- Create: `.clang-format` (repo root)

**Interfaces:**
- Produces: `.clang-format` using LLVM base with 2-space indent, matching the style referenced in `CONTRIBUTING.md`.

- [ ] **Step 1: Create .clang-format**

Write the following to `.clang-format`:
```yaml
---
BasedOnStyle: LLVM
IndentWidth: 2
ColumnLimit: 100
PointerAlignment: Left
SpaceAfterCStyleCast: false
BreakBeforeBraces: Attach
AllowShortFunctionsOnASingleLine: Inline
SortIncludes: false
```

- [ ] **Step 2: Verify it parses**

Run: `clang-format --style=file --dump-config 2>&1 | head -5`
Expected: outputs config without error (requires clang-format installed locally; skip if not available, CI will catch it).

- [ ] **Step 3: Commit**

```bash
git add .clang-format
git commit -m "chore: add .clang-format (LLVM base, 2-space indent, 100 col)"
```

---

## Task 4: Harden CI — add clang-format check and cppcheck

**Files:**
- Modify: `.github/workflows/unit-tests.yml`

**Interfaces:**
- Consumes: `.clang-format` from Task 3
- Produces: two additional CI jobs: `lint-format` and `static-analysis`

`CONTRIBUTING.md` promises three CI gates (Arduino compile, cppcheck, clang-format) but the workflow only runs unit tests. This task first auto-formats all existing source so CI is green from day one, then adds the two missing code-quality jobs. Arduino compile is excluded — it requires an ESP32 toolchain and significantly inflates CI minutes; note the omission in the job name.

- [ ] **Step 1: Auto-fix all existing clang-format violations**

This must run before Step 2 (adding the CI gate) so the gate doesn't immediately fail on existing code.

```bash
find main/src -name '*.cpp' -o -name '*.h' | xargs clang-format --style=file -i
```

Review the diff — formatting-only changes expected (indentation, spacing, brace style). No logic changes.

```bash
git diff --stat
```

Commit the formatting pass:
```bash
git add main/src/
git commit -m "style: apply clang-format to all source files"
```

- [ ] **Step 2: Rewrite unit-tests.yml**

Replace the full contents of `.github/workflows/unit-tests.yml` with:

```yaml
name: CI

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install CMake
        run: sudo apt-get install -y cmake

      - name: Configure
        run: cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build tests/build --parallel

      - name: Run tests
        run: ctest --test-dir tests/build --output-on-failure --parallel 4

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: test-results
          path: tests/build/Testing/

  lint-format:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install clang-format
        run: sudo apt-get install -y clang-format

      - name: Check formatting
        run: |
          find main/src -name '*.cpp' -o -name '*.h' | \
            xargs clang-format --style=file --dry-run --Werror

  static-analysis:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install cppcheck
        run: sudo apt-get install -y cppcheck

      - name: Run cppcheck
        run: |
          cppcheck \
            --error-exitcode=1 \
            --suppress=missingIncludeSystem \
            --suppress=unmatchedSuppression \
            --inline-suppr \
            -I main/src \
            main/src/ 2>&1
```

- [ ] **Step 3: Verify YAML syntax**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/unit-tests.yml'))" && echo "OK"`
Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/unit-tests.yml
git commit -m "ci: add clang-format and cppcheck jobs"
```

---

## Task 5: Add GitHub community files

**Files:**
- Create: `SECURITY.md`
- Create: `CODE_OF_CONDUCT.md`
- Create: `.github/ISSUE_TEMPLATE/bug_report.md`
- Create: `.github/ISSUE_TEMPLATE/feature_request.md`
- Create: `.github/pull_request_template.md`

**Interfaces:**
- Produces: standard OSS community health files

- [ ] **Step 1: Create SECURITY.md**

Write to `SECURITY.md`:
```markdown
# Security Policy

## Supported Versions

Only the latest commit on `main` receives security fixes.

## Reporting a Vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

Email the maintainer at the address listed in your GitHub profile, or open a
[GitHub Security Advisory](../../security/advisories/new) (private disclosure).

Include:
- A clear description of the vulnerability
- Steps to reproduce
- Potential impact
- Any suggested remediation

You will receive an acknowledgement within 72 hours. If the vulnerability is
confirmed, a fix will be issued as soon as practical and credited to the
reporter unless anonymity is requested.

## Out of Scope

- Vulnerabilities in dependencies (nanopb, ESP-IDF) should be reported upstream.
- Physical hardware attacks (JTAG, UART, SPI sniffing) are by design accessible
  on ESP32 developer boards and outside the scope of this policy.
```

- [ ] **Step 2: Create CODE_OF_CONDUCT.md**

Write to `CODE_OF_CONDUCT.md` using the Contributor Covenant 2.1 short form:
```markdown
# Contributor Covenant Code of Conduct

## Our Pledge

We as contributors and maintainers pledge to make participation in this project
a harassment-free experience for everyone, regardless of age, body size,
disability, ethnicity, gender identity and expression, level of experience,
nationality, personal appearance, race, religion, or sexual identity and
orientation.

## Our Standards

**Positive behaviour includes:**
- Using welcoming and inclusive language
- Being respectful of differing viewpoints
- Gracefully accepting constructive criticism
- Focusing on what is best for the community

**Unacceptable behaviour includes:**
- Trolling, insulting/derogatory comments, or personal attacks
- Public or private harassment
- Publishing others' private information without permission
- Other conduct which could reasonably be considered inappropriate

## Enforcement

Project maintainers are responsible for clarifying and enforcing standards.
Instances of unacceptable behaviour may be reported by opening a GitHub issue
or contacting the maintainer directly. All complaints will be reviewed and
investigated promptly.

This Code of Conduct is adapted from the
[Contributor Covenant v2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/).
```

- [ ] **Step 3: Create .github/ISSUE_TEMPLATE/bug_report.md**

```markdown
---
name: Bug report
about: Something is broken
labels: bug
---

## Describe the bug
A clear and concise description of what the bug is.

## To reproduce
Steps to reproduce the behaviour:
1. 
2. 

## Expected behaviour
What you expected to happen.

## Environment
- Board: (e.g. ESP32-WROOM-DA)
- Arduino IDE version:
- ESP32 core version:
- Commit/tag:

## Serial output or error codes
Paste relevant serial output or seven-segment error codes here.
```

- [ ] **Step 4: Create .github/ISSUE_TEMPLATE/feature_request.md**

```markdown
---
name: Feature request
about: Suggest an idea for a new adapter, protocol extension, or tooling improvement
labels: enhancement
---

## Problem / motivation
What problem does this solve? Why is it needed?

## Proposed solution
Describe what you'd like.

## Alternatives considered
Any other approaches you thought about?

## Additional context
Links, references, hardware datasheets, etc.
```

- [ ] **Step 5: Create .github/pull_request_template.md**

```markdown
## Summary
<!-- What does this PR do? One paragraph max. -->

## Type of change
- [ ] Bug fix
- [ ] New adapter
- [ ] Protocol change
- [ ] Documentation
- [ ] CI/tooling
- [ ] Other:

## Checklist
- [ ] `clang-format --style=file` applied — `git diff --check` clean
- [ ] No `new` / `malloc` after `setup()` (Tiger Style memory policy)
- [ ] All errors route through `src/error/Error.h`
- [ ] Unit tests added / updated for any logic change
- [ ] Documentation updated if behaviour changes
- [ ] `project_config.h` example MACs remain as `AA:BB:CC:...` placeholders

## Testing done
<!-- How did you verify this? Hardware tested? Unit tests? -->
```

- [ ] **Step 6: Commit**

```bash
git add SECURITY.md CODE_OF_CONDUCT.md \
  .github/ISSUE_TEMPLATE/bug_report.md \
  .github/ISSUE_TEMPLATE/feature_request.md \
  .github/pull_request_template.md
git commit -m "chore: add OSS community health files (SECURITY, CoC, templates)"
```

---

## Task 6: Rewrite README.md

**Files:**
- Modify: `README.md` (full rewrite)

**Interfaces:**
- Consumes: actual file layout on disk (verified above)
- Produces: accurate, complete README with no duplicate sections, correct paths, enrollment workflow, nanopb mention

**Known issues to fix:**
1. Duplicate "Project Structure" section (appears twice)
2. Structure lists `src/utils/EEPROM_Manager` but actual path is `src/persistence/EEPROM_Manager`
3. Mentions `ProtobufCodec`, `MessageRouter`, `PeerManager`, `ConfigurationManager`, `NetworkManager` in `src/utils/` — these classes do not exist
4. No mention of enrollment/provisioning (Curve25519 keypair, PLANETOPIA_PUBKEY serial output, JOIN_ACK)
5. No mention of nanopb (serialization library now used)
6. No mention of replay cache, WDT protection
7. `<repository-url>` placeholder in setup instructions
8. License section says "[Add your license information here]"
9. Version history stops at v1.3.0 — misses nanopb migration, enrollment

- [ ] **Step 1: Write the new README.md**

Write the following to `README.md`:

````markdown
# Planetopia — ESP-NOW Mesh Network Firmware

[![CI](https://github.com/YOUR_USERNAME/Planetopia-nodes/actions/workflows/unit-tests.yml/badge.svg)](https://github.com/YOUR_USERNAME/Planetopia-nodes/actions/workflows/unit-tests.yml)
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
    │   ├── Error.h                 # Public `planetopia::err::fail / fatal` helpers
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
git clone https://github.com/YOUR_USERNAME/Planetopia-nodes.git
cd Planetopia-nodes
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
PLANETOPIA_PUBKEY:3a7f2b...
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
````

- [ ] **Step 2: Verify no stale references remain**

Run:
```bash
grep -n "repository-url\|Add your license\|src/utils/ProtobufCodec\|src/utils/MessageRouter\|src/utils/PeerManager\|src/utils/ConfigurationManager\|src/utils/NetworkManager" README.md
```
Expected output: (empty — no matches)

- [ ] **Step 3: Verify no duplicate Project Structure section**

Run:
```bash
grep -c "Project Structure" README.md
```
Expected output: `1`

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: rewrite README — accurate structure, enrollment, nanopb, license"
```

---

## Task 7: Replace REFACTORING_GUIDE.md with accurate architecture guide

**Files:**
- Modify: `REFACTORING_GUIDE.md` (full rewrite)

**Why:** The current document describes five classes (`MessageRouter`, `PeerManager`, `ProtobufCodec`, `ConfigurationManager`, `NetworkManager`) as existing in `src/utils/`. They do not exist. A new contributor who reads this will be misled. Replace with an accurate architecture reference.

- [ ] **Step 1: Write the new REFACTORING_GUIDE.md**

```markdown
# Architecture Guide

This document describes the design principles and module responsibilities of
the Planetopia firmware. It replaces an earlier draft that described planned
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
- `Error.h`: public API — `planetopia::err::fail()` / `planetopia::err::fatal()`.
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
```

- [ ] **Step 2: Verify no references to non-existent classes remain**

Run:
```bash
grep -n "MessageRouter\|PeerManager\|ProtobufCodec\|ConfigurationManager\|NetworkManager\|src/utils/" REFACTORING_GUIDE.md
```
Expected output: (empty)

- [ ] **Step 3: Commit**

```bash
git add REFACTORING_GUIDE.md
git commit -m "docs: replace stale refactor guide with accurate architecture reference"
```

---

## Task 8: Update docs/server_requirements.md — add enrollment protocol

**Files:**
- Modify: `docs/server_requirements.md`

**Why:** The enrollment protocol (public key announcement, JOIN_ACK) is fully implemented in firmware but absent from the server requirements. Any server integration will silently fail for new nodes until this is documented.

- [ ] **Step 1: Append the enrollment section**

At the end of `docs/server_requirements.md`, before the final `---` or at the very end, append:

```markdown
---

### Enrollment Protocol (node provisioning)

When a node boots for the first time (or after an EEPROM wipe) it is
**unenrolled**. An unenrolled node:

1. Prints its Curve25519 public key to serial (master node only relays):
   ```
   PLANETOPIA_PUBKEY:3a7f2b...  (64 hex chars = 32 bytes)
   ```
2. Broadcasts `MESH_TYPE_ENROLLMENT` (type=2) messages every 10 seconds with
   `enrollmentPublicKey` populated.
3. Refuses to forward sensor data until it receives a `MESH_TYPE_JOIN_ACK`.

**Server responsibilities for enrollment:**

1. **Detect enrollment frames** — watch for incoming frames with `messageType == 2`.
   Extract the 32-byte `enrollmentPublicKey` and the `originMacAddress`.

2. **Approve the node** — your server decides whether to admit the node (user
   confirmation, allowlist check, etc.).

3. **Send JOIN_ACK** — construct a `MeshMessage` and write it to serial:
   ```go
   m := &mesh.MeshMessage{
     MessageType:      4,               // MESH_TYPE_JOIN_ACK
     DataType:         3,               // SERIAL_ADAPTER
     TargetMacAddress: nodeMac[:],      // 6-byte MAC of the unenrolled node
     Data:             make([]byte, 12), // all zeros; content ignored
   }
   writeFrame(port, m)
   ```
   The master relays this to the target node over the mesh.

4. **Node becomes enrolled** — the node sets its enrolled flag in EEPROM and
   begins forwarding sensor data. It will no longer broadcast enrollment frames.

**Key points:**
- The server should persist approved public keys (keyed by MAC address) for
  future session continuity checks.
- If the serial framing drops the JOIN_ACK the node will retry every 10 seconds.
- `enrollmentPublicKey` is zero-filled (`\x00` × 32) in all non-enrollment messages.

#### Enrollment message flow

```
[node boot]
    Node → (ESP-NOW broadcast) → Master: MESH_TYPE_ENROLLMENT (pubkey inside)
    Master → (serial frame) → Server: forwards enrollment frame

[server approves]
    Server → (serial frame) → Master: MESH_TYPE_JOIN_ACK (targetMAC = node MAC)
    Master → (ESP-NOW unicast) → Node: JOIN_ACK

[node enrolled]
    Node → begins sending MESH_TYPE_ADAPTER_DATA
```
```

- [ ] **Step 2: Verify the section was appended**

Run: `grep -c "JOIN_ACK" docs/server_requirements.md`
Expected: at least `3`

- [ ] **Step 3: Commit**

```bash
git add docs/server_requirements.md
git commit -m "docs: add enrollment protocol section to server requirements"
```

---

## Task 9: Fix docs/adapter_development_guide.md

**Files:**
- Modify: `docs/adapter_development_guide.md`

**Known issues:**
1. `#include "src/utils/Logger.h"` — actual path is `src/core/Logger.h`
2. Direct `EEPROM.write()` in "Method 1: Change default" — use `EEPROM_Manager` instead
3. References `Logger::logln(LogLevel::LOG_INFO, ...)` — actual signature is `Logger::logln(const char*, const char*, LogLevel)`

- [ ] **Step 1: Fix the Logger include paths**

In `docs/adapter_development_guide.md`, replace all occurrences of:
```
#include "src/utils/Logger.h"
```
with:
```
#include "src/core/Logger.h"
```
(Two occurrences — one in the template block, one in the example block.)

- [ ] **Step 2: Fix Method 1 — change default (direct EEPROM → EEPROM_Manager)**

Find the block starting with:
```cpp
void AdapterFactory::initializeDefaultsIfUnset() {
    // Check if adapter type is already set
    if (EEPROM.read(ADAPTER_TYPE_ADDR_FACTORY) == 0xFF) {
        // Set default adapter type (change this line for testing)
        EEPROM.write(ADAPTER_TYPE_ADDR_FACTORY, TEMP_ADAPTER);  // Changed from PIR_ADAPTER
        EEPROM.commit();
        Logger::logln(LogLevel::LOG_INFO, "Set default adapter type to TEMP_ADAPTER");
    }
}
```

Replace with:
```cpp
// Do not call this method directly — change DEFAULT_ADAPTER in project_config.h instead.
// AdapterFactory::initializeDefaultsIfUnset() reads from EEPROM_Manager, which already
// seeds from project_config.h::DEFAULT_ADAPTER on a blank device.
```

- [ ] **Step 3: Fix Logger call signatures in debugging section**

Find:
```cpp
Logger::logln(LogLevel::LOG_INFO, "Temp_Adapter: Temperature read: %.1f", _lastTemperature);
```

Replace with:
```cpp
// Logger::logln takes (tag, message, level). For formatted strings, use String():
Logger::logln("TEMP", ("Temp_Adapter: Temperature read: " + String(_lastTemperature, 1)).c_str(), LogLevel::LOG_INFO);
```

- [ ] **Step 4: Verify no stale Logger path remains**

Run: `grep -n "src/utils/Logger" docs/adapter_development_guide.md`
Expected output: (empty)

- [ ] **Step 5: Commit**

```bash
git add docs/adapter_development_guide.md
git commit -m "docs: fix Logger path and patterns in adapter development guide"
```

---

## Task 10: Update CONTRIBUTING.md — align with actual CI

**Files:**
- Modify: `CONTRIBUTING.md:44-51`

**Why:** The current text says CI runs `arduino-cli compile`, `cppcheck`, and `clang-format`. After Task 4, cppcheck and clang-format are now true. Arduino compile is not in CI (toolchain cost). Update the claim.

- [ ] **Step 1: Update CI section**

Find in `CONTRIBUTING.md`:
```markdown
## 6. CI pipeline (GitHub Actions)
The workflow runs automatically:
```
feat arduino-cli compile (ESP32 + strict warnings)
- cppcheck (MISRA subset)
- clang-format check
```
PR merges are blocked until the workflow is green.
```

Replace with:
```markdown
## 6. CI pipeline (GitHub Actions)

The workflow runs automatically on every push and PR:

- **unit-tests** — CMake build + CTest (Linux native, no ESP32 toolchain needed)
- **lint-format** — `clang-format --dry-run --Werror` over all `main/src/*.{h,cpp}`
- **static-analysis** — `cppcheck` with `--error-exitcode=1`

PR merges are blocked until all three jobs are green.

> **Note:** Arduino / ESP32 toolchain compilation is not in CI (large binary
> download, ~10 min). Run `arduino-cli compile --fqbn esp32:esp32:esp32da main`
> locally before submitting a PR that touches firmware source.
```

- [ ] **Step 2: Verify update**

Run: `grep -n "arduino-cli compile (ESP32" CONTRIBUTING.md`
Expected output: (empty — old text removed)

- [ ] **Step 3: Commit**

```bash
git add CONTRIBUTING.md
git commit -m "docs: align CONTRIBUTING CI section with actual workflow"
```

---

## Self-Review

Checking coverage:

| Requirement | Task |
|-------------|------|
| LICENSE file | Task 1 |
| Real MACs removed | Task 2 |
| .clang-format present | Task 3 |
| CI matches CONTRIBUTING.md claims | Task 4, Task 10 |
| SECURITY.md | Task 5 |
| CODE_OF_CONDUCT.md | Task 5 |
| Issue/PR templates | Task 5 |
| README accurate (no stale paths/classes) | Task 6 |
| REFACTORING_GUIDE accurate | Task 7 |
| Enrollment protocol documented | Task 8 |
| Adapter guide has correct Logger path | Task 9 |

**Placeholder scan:** No TBD, TODO (except in `DEFAULT_PEERS` comment which is intentional), or "implement later" patterns in task steps.

**Type consistency:** No inter-task function signatures; tasks are independent file edits.
