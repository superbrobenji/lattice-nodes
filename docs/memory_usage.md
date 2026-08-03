# ESP32 Memory & Storage Usage

Board: `esp32:esp32:esp32` (ESP32 Dev Module). Toolchain: `arduino-cli` + `esp32:esp32@3.3.10`.

> **Status (2026-07-22): the hard numbers below are a stale baseline.**
> They were measured **2026-07-13** — *before* the repo restructure (#24, 07-14)
> and *before* Phases 1–5 (#33, #34, #35, #40, #41) landed end-to-end AEAD crypto,
> the neighbor/route tables, dual-master failover, and the replay cache. A clean
> firmware re-measure is currently **blocked** (see [Re-measurement is blocked](#re-measurement-is-currently-blocked));
> the [static-allocation](#fixed-ram-allocations-current-source) and
> [EEPROM](#eeprom-nvs-persistent-storage) sections are computed from *current* source and are live.

## Baseline Summary (measured 2026-07-13 — STALE)

| Region | Used | Total | % |
|---|---|---|---|
| Flash (program storage) | 972,596 B (950 KB) | 1,310,720 B (1,280 KB) | **74%** |
| Static RAM (globals/BSS) | 48,532 B (47 KB) | 327,680 B (320 KB) | **14%** |

Phases 1–5 have grown both figures (see [Changes since baseline](#changes-since-baseline-phases-15)).
Static RAM is estimated ~+5.7 KB; flash is the region to watch — re-measure before trusting the 74%.

## Flash Breakdown (baseline)

### By ELF section

| Section | Size | Purpose |
|---|---|---|
| `.flash.text` | 692 KB | Code in flash |
| `.flash.rodata` | 152 KB | Constants, strings, lookup tables |
| `.iram0.text` | 81 KB | Time-critical code mapped to IRAM |
| `.iram0.vectors` | 1 KB | Interrupt vectors |
| **Free** | **~338 KB** | 26% remaining |

### By subsystem (`.text` symbols)

| Subsystem | Flash |
|---|---|
| WiFi / ESP-NOW / lwIP stack | ~131 KB |
| mbedTLS (ECDH, SHA-256, entropy, CTR-DRBG) | ~50 KB |
| Application code (`lattice` namespace) | ~40 KB |
| BT symbols (compiled-in despite `btStop()`) | ~10 KB |
| nanopb | ~2.4 KB |
| ESP-IDF SDK / FreeRTOS / HAL / misc | ~468 KB |

## Static RAM Breakdown (baseline)

### By ELF section

| Section | Size | Purpose |
|---|---|---|
| `.dram0.data` | 23.5 KB | Initialized globals |
| `.dram0.bss` | 23.9 KB | Zero-init globals |
| `.rtc.force_slow` | 32 B | RTC slow RAM |
| **Free for stack + heap** | **~279 KB** | 86% remaining |

### By subsystem (`.data` + `.bss` symbols)

| Subsystem | Static RAM |
|---|---|
| FreeRTOS / system | ~5.4 KB |
| WiFi / Net static globals | ~4.0 KB |
| Application globals (`mesh`, `adapter`, LEDs, etc.) | ~2.7 KB |
| mbedTLS static globals | ~1.6 KB |

At baseline the `mesh` object was the largest single app symbol at **868 B** BSS.
Phases 2–5 have since grown it to an estimated **~5.7 KB** — see below.

## Changes since baseline (Phases 1–5)

Landed after the 07-13 measure. Directional flash/RAM impact (not yet re-measured):

| Phase | Feature | Flash | Static RAM |
|---|---|---|---|
| 1 (#33) | Protocol v3 + end-to-end AEAD payload crypto (X25519 + AEAD) | **↑↑** new cipher + KDF paths in mbedTLS | `E2EKeyStore` ~710 B |
| 2 (#34) | Multi-hop data uplink via `NeighborTable` | ↑ small | `NeighborTable` ~96 B |
| 3 (#35) | Downlink source routing + `k_down` sealing | ↑ | `RouteTable` ~2.3 KB (master-side) + downlink LRU 24 B |
| 4 (#40) | Dual-master data failover | ↑ | secondary-master MACs/keys, small |
| 5 (#41) | Dual-master `CONFIG_SET`, relayed-target, nanopb regen | ↑ | proto fields widen `mesh_message` |
| (#28/#23) | Replay protection + route reporting | ↑ | `ReplayCache` ~206 B |

**Net:** static RAM is still trivial against the 320 KB budget. **Flash is the metric that
actually moved** — the AEAD cipher, larger protobuf message, and dual-master paths all add
`.text`/`.rodata`. The 74% figure is optimistic now; re-measure before the next crypto/adapter
addition.

## Fixed RAM allocations (current source)

Tiger-style: every mesh data structure is a fixed-size array sized from `main/project_config.h`
— **zero heap, allocated up-front whether or not a node uses it.** Computed from field layout
(xtensa 4-byte alignment). All live inside the single `mesh` global (`main/src/mesh/Mesh.h`):

| Structure | Bound (config) | Per entry | Total | Notes |
|---|---|---|---|---|
| `recvQueue` (RX ring) | `RECV_QUEUE_SIZE = 8` | 6 + 242 (`mesh_message`) | **~1.94 KB** | SPSC lock-free queue, WiFi-cb → loop |
| `RouteTable` | `LATTICE_ROUTE_TABLE_MAX = 32` | 72 (incl. `path[60]`) | **~2.25 KB** | **Master-only use; dead weight on leaf nodes** |
| `E2EKeyStore` | `LATTICE_E2E_KEYCACHE_MAX = 10` | 71 (`kUp[32]`+`kDown[32]`) | **~710 B** | Derived-key cache, X25519 |
| `PeerRegistry` | `MAX_PEERS = 10` | 44 (`publicKey[32]`) | **~440 B** | Mirrors EEPROM peer list |
| `ReplayCache` | `CACHE_SIZE = 16` | 12 | **~206 B** | Anti-replay (origin, epoch, seq) |
| `NeighborTable` | `LATTICE_NEIGHBOR_MAX = 8` | 12 | **~96 B** | Uplink next-hop candidates |
| `downlinkPeerLru` | `LATTICE_DOWNLINK_PEER_MAX = 4` | 6 | **24 B** | Bounds ESP-NOW peer-table exhaustion |
| misc (MACs, `meshKey[16]`, fwd peer, scalars) | — | — | **~60 B** | |
| **`mesh` object total** | | | **~5.7 KB** | vs 868 B at baseline |

`mesh_message` is the in-RAM wire struct from the vendored `lattice-protocol/c` (242 B,
`static_assert`-locked to the server proto), aliased in via `using ::mesh_message`. The `recvQueue`
and any per-frame stack copies inherit its 242 B — the single largest lever on both RAM and stack
depth.

## EEPROM / NVS (persistent storage)

Layout in `main/src/persistence/EepromManager.h`. `EEPROM.begin(TOTAL_SIZE)` maps a 512-byte NVS blob.

| Region | Addr | Size |
|---|---|---|
| MASTER_FLAG / DEV_FLAG / ADAPTER_TYPE | 0,1,8 | 3 B |
| MESH_KEY | 16 | 16 B |
| PEER_LIST (10 × 38 B: MAC + pubkey) | 32 | 380 B |
| REBOOT_REASON / COUNT + reserved | 412 | 5 B |
| PRIVATE_KEY / PUBLIC_KEY / KEYPAIR_CRC | 417 | 66 B |
| ENROLLED_FLAG / BOOT_EPOCH | 483 | 5 B |
| KNOWN_MASTER_MAC (primary) | 488 | 6 B |
| SCHEMA_VERSION / TX_POWER / NODE_ID | 494 | 3 B |
| KNOWN_MASTER_MAC_SECONDARY | 497 | 6 B |
| **Total used** | | **503 / 512 B (98%)** |

> **EEPROM is the tightest budget in the whole system — 9 bytes free.**
> `PEER_LIST` (380 B) dominates: raising `MAX_PEERS` past 10 overflows 512 B immediately
> (each peer = 38 B). Any new persisted field needs the 3 reserved bytes (414–416) or an
> `EEPROM.begin()` size bump + a schema-version migration (see `EepromManager::init()`).

## Runtime Heap

Runtime-only, not in the static figures. At steady state after `setup()`:

- WiFi stack claims ~80–100 KB heap
- FreeRTOS task stacks claim additional space
- Effective usable heap for application: **~150–180 KB**

E2E AEAD (Phase 1) runs X25519 on the stack per key derivation (~ms); the `E2EKeyStore` exists
specifically to amortise that so it isn't re-run per frame.

## Re-measurement is currently BLOCKED

The firmware **does not build cleanly under `arduino-cli`** (only host unit/e2e tests and CodeQL
build in CI — no CI job compiles the firmware). Three issues block a fresh `xtensa-esp32-elf-size`
measure; the Arduino IDE hides all three via different include resolution:

1. **nanopb include path** — `mesh.pb.h` does `#include "pb.h"` but `.../serialization/nanopb`
   isn't on the default sketch include path. Fix: add it, or flatten nanopb into the sketch.
2. **Non-self-contained headers** — `main/src/app/ButtonHandler.h` uses `Logger` / `LogLevel`
   unqualified, relying on a `using namespace lattice::utils;` that `main.ino` declares *before*
   including it. (The baseline doc noted the same class of bug for `Serial_Adapter`; it moved in
   the restructure, not fixed.) Fix: each header includes what it uses and fully-qualifies names.
3. **Duplicate `mesh_message` typedef** — the vendored `main/lib/lattice-protocol/c/mesh_message.h`
   is auto-scanned from `lib/` and its `typedef struct … mesh_message` collides with the alias
   pulled in via `using ::mesh_message`. Fix: keep the vendored C tree out of the Arduino
   `lib/` auto-scan, or gate its inclusion.

**Prerequisite for accurate optimization work:** fix 1–3 so `arduino-cli compile --fqbn
esp32:esp32:esp32` succeeds, then add a CI firmware-build + size-report job so this doc can't
silently rot again. Re-run:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/fw main
xtensa-esp32-elf-size -A build/fw/main.ino.elf
```

## Optimization Plan

Prioritised by (constraint pressure × payoff). Do the measurement-unblock first — everything
below is estimated until the firmware compiles under `arduino-cli`.

### P0 — Unblock measurement (prerequisite)
- Fix the three build blockers above; add a CI size-report job. **Without this every number here
  is a guess.** Low effort, unlocks everything else.

### P1 — Flash (the real constraint, ~74%→higher)
| Lever | Est. saving | Effort | Risk |
|---|---|---|---|
| **Drop BT** — `CONFIG_BT_ENABLED=n`. Needs an ESP-IDF (not Arduino) build; `btStop()` only disables at runtime. | ~10 KB + heap | High (toolchain switch) | Med |
| **Trim mbedTLS** — Phase-1 AEAD pulled in a cipher; prune unused suites via `mbedtls_config` (drop RSA/DHE/unused curves, keep X25519 + the one AEAD). | 10–30 KB | Med (custom config on IDF) | Med |
| **Strip logging in prod** — `DEFAULT_LOG_LEVEL = LOG_NONE` already; ensure format strings/`.rodata` are compiled out, not just gated at runtime (`#if` not `if`). | few–10 KB `.rodata` | Low | Low |
| **`-Os` + LTO + `-ffunction-sections,-Wl,--gc-sections`** if not already set by the core. | 5–15 KB | Low | Low |
| **Single AEAD** — if Phase 1 links both AES-GCM and ChaCha20-Poly1305, pick one. | 5–15 KB | Low | Low |

### P2 — EEPROM (98% full — hardest ceiling)
| Lever | Payoff | Effort |
|---|---|---|
| **Shrink `PEER_LIST`** — 380 B / 512 is the whole squeeze. Store a peer-count + packed records instead of a fixed 10×38 B, or move peers to a dedicated NVS namespace and free the raw-EEPROM budget. | frees ~350 B | Med (migration) |
| **Migrate raw EEPROM → NVS/Preferences** — key/value NVS avoids the 512 B fixed map and versioning gymnastics entirely. | removes the ceiling | Med-High (rewrite `EepromManager`) |
| Use the 3 reserved bytes (414–416) for the *next* small field only — not a growth strategy. | 3 B | trivial |

### P3 — Static RAM (~5.7 KB mesh object; not urgent, but wasteful)
| Lever | Est. saving | Notes |
|---|---|---|
| **`RouteTable` on masters only** — 2.25 KB is allocated on every leaf but only the master routes downlink. Gate behind role (compile-time or a pointer allocated on promotion). | ~2.25 KB on leaves | Biggest single RAM win; needs role known before construction |
| **Right-size the bounds** — `LATTICE_ROUTE_TABLE_MAX=32`, `RECV_QUEUE_SIZE=8`, `CACHE_SIZE=16` are generous. Tune to real deployment fan-out. | 0.5–2 KB | Measure real occupancy first |
| **Shrink `mesh_message` residency** — 242 B copied into an 8-deep ring (~1.94 KB) and onto the stack per frame. If most fields are optional, a compact internal form or a smaller ring would cut both RAM and worst-case stack. | up to ~1 KB | Touches wire struct — coordinate with server proto `static_assert` |

### P4 — Hygiene (enables the above, no direct saving)
- Make headers self-contained (blocker #2) — also the correctness fix for non-Arduino toolchains.
- Keep the vendored `lattice-protocol` submodule out of Arduino `lib/` auto-scan (blocker #3).
- **Re-run this analysis after every major dependency/feature change** and commit the diff, so
  flash %, the mesh-object RAM total, and EEPROM headroom are always current.

## Notes

- **Flash is the pressure point; EEPROM is the hard ceiling (98%).** RAM has huge headroom.
- **BT ~10 KB flash** despite `btStop()` — compiled in, runtime-disabled only. Removing needs an
  ESP-IDF build (`CONFIG_BT_ENABLED=n`), unavailable in the Arduino toolchain.
- Baseline flash/RAM figures are **2026-07-13, pre-Phase 1–5** — treat as a floor, not current.
