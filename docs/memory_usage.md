# ESP32 Firmware Memory & Flash Usage

Target: `esp32:esp32` (original ESP32, Xtensa dual-core). Toolchain: **ESP-IDF v5.5.1**
(pinned in `firmware/dependencies.lock`), driven via `idf.py`.

## 1. Status (measured 2026-08-11)

**Measurement is not blocked.** ESP-IDF has been this repo's build system since well
before the current refactor phase — a previous version of this document described an
`arduino-cli`-only toolchain with three build blockers and a "re-measurement is
currently BLOCKED" framing. That framing predates the ESP-IDF migration and is stale;
it has been removed rather than carried forward.

Every number in this document was copied verbatim from a real, successful
`idf.py build` + `idf.py size` + `idf.py size-components` run against the current tree
on **2026-08-11**, or is a `firmware/main/project_config.h` constant confirmed by
reading the current source during that same research pass. Nothing below is estimated,
and nothing is reused from the old 2026-07-13 `arduino-cli` baseline — that baseline
measured a different toolchain against a much earlier tree (pre-repo-restructure,
pre-Phase-1-5) and is no longer a meaningful comparison point.

## 2. Build prerequisite: `master_pubkey_pin.h`

A first `idf.py build` on a fresh checkout **fails** unless
`firmware/main/config/master_pubkey_pin.h` already exists. This is a deliberate,
intentional `#error` gate, not a bug:

```
firmware/main/config/master_pubkey_pin_wrapper.h:11:4: error: "firmware/main/config/master_pubkey_pin.h not found.
Generate it via tools/gen_master_pubkey_pin.py or build with -DLATTICE_ALLOW_EXAMPLE_PIN=1 (DEV_MODE only)."
```

The file compiles the hub's real master Curve25519 public key + MAC into the firmware
so nodes can authenticate `JOIN_ACK` against a pinned identity. It is gitignored and
per-deployment — generate it with:

```bash
python3 tools/gen_master_pubkey_pin.py <path-to-masterkey.json> <master-mac-address>
```

See `docs/getting_started.md` for the full first-build walkthrough (where to get
`masterkey.json`, the `DEV_MODE`-only `-DLATTICE_ALLOW_EXAMPLE_PIN=1` escape hatch, and
why it isn't shippable) — not duplicated here. The measurements below were taken after
generating this header with a throwaway, non-production key; the generated header is a
fixed-size `constexpr uint8_t[32]`/`uint8_t[6]` pair regardless of key content, so it
does not distort the flash/RAM numbers that follow.

## 3. Flash usage

### Summary (`idf.py size`, verbatim)

```
                             Memory Type Usage Summary
┏━━━━━━━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━┳━━━━━━━━━━┳━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━┓
┃ Memory Type/Section   ┃ Used [bytes] ┃ Used [%] ┃ Remain [bytes] ┃ Total [bytes] ┃
┡━━━━━━━━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━╇━━━━━━━━━━╇━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━━━┩
│ Flash Code            │       471364 │          │                │               │
│    .text              │       471364 │          │                │               │
│ IRAM                  │       101111 │    77.14 │          29961 │        131072 │
│    .text              │       100083 │    76.36 │                │               │
│    .vectors           │         1028 │     0.78 │                │               │
│ Flash Data            │        76628 │          │                │               │
│    .rodata            │        76372 │          │                │               │
│    .appdesc           │          256 │          │                │               │
│ DRAM                  │        44084 │    24.39 │         136652 │        180736 │
│    .bss               │        29088 │    16.09 │                │               │
│    .data              │        14996 │      8.3 │                │               │
│ RTC SLOW              │           56 │     0.68 │           8136 │          8192 │
│    .force_slow        │           32 │     0.39 │                │               │
│    .rtc_slow_reserved │           24 │     0.29 │                │               │
└───────────────────────┴──────────────┴──────────┴────────────────┴───────────────┘
Total image size: 664131 bytes (.bin may be padded larger)
```

**Plain-English summary:**

| Metric | Value |
|---|---|
| Flash Code (`.text`) | 471,364 B (~460 KB) |
| Flash Data (`.rodata` + `.appdesc`) | 76,628 B (~74.8 KB) |
| Total image size | 664,131 B |
| App `.bin` file | `0xa22b0` bytes (664,240 B) |
| Smallest app partition (`factory`, per `partitions.csv`) | `0x100000` bytes (1 MiB) |
| Free in that partition | `0x5dd50` bytes (384,336 B) — **37% free**, i.e. the app uses 63% of its 1 MiB partition |
| Bootloader binary | `0x6680` bytes (26,240 B), `0x980` bytes (2,432 B / 8%) free in its own region |

(The 109-byte gap between "Total image size" 664,131 B and the `.bin` file's 664,240 B
is padding — the tool's own note above says the `.bin` "may be padded larger.")

### Per-library breakdown (`idf.py size-components`, verbatim)

```
                                                                              Per-archive contributions to ELF file
┏━━━━━━━━━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━┳━━━━━━━┳━━━━━━━┳━━━━━━━┳━━━━━━━┳━━━━━━━┳━━━━━━━━━━┳━━━━━━━━━━━━┳━━━━━━━━┳━━━━━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━┳━━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━┓
┃ Archive File            ┃ Total Size ┃  DRAM ┃  .bss ┃ .data ┃  IRAM ┃ .text ┃ .vectors ┃ Flash Code ┃  .text ┃ Flash Data ┃ .rodata ┃ .appdesc ┃ RTC SLOW ┃ .rtc_slow_reserved ┃ .force_slow ┃
┡━━━━━━━━━━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━╇━━━━━━━╇━━━━━━━╇━━━━━━━╇━━━━━━━╇━━━━━━━╇━━━━━━━━━━╇━━━━━━━━━━━━╇━━━━━━━━╇━━━━━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━╇━━━━━━━━━━╇━━━━━━━━━━━━━━━━━━━━╇━━━━━━━━━━━━━┩
│ libnet80211.a           │     138325 │  8822 │  7968 │   854 │  5256 │  5256 │        0 │     122721 │ 122721 │       1526 │    1526 │        0 │        0 │                  0 │           0 │
│ libpp.a                 │      69175 │  4183 │  1544 │  2639 │ 21802 │ 21802 │        0 │      41881 │  41881 │       1309 │    1309 │        0 │        0 │                  0 │           0 │
│ libmbedcrypto.a         │      67635 │    49 │    25 │    24 │     0 │     0 │        0 │      54924 │  54924 │      12662 │   12662 │        0 │        0 │                  0 │           0 │
│ liblwip.a               │      65290 │  2491 │  2479 │    12 │     0 │     0 │        0 │      59904 │  59904 │       2895 │    2895 │        0 │        0 │                  0 │           0 │
│ libphy.a                │      46222 │  3074 │   635 │  2439 │  8998 │  8998 │        0 │      34150 │  34150 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libefuse.a              │      44852 │    64 │     4 │    60 │     0 │     0 │        0 │        909 │    909 │      43879 │   43879 │        0 │        0 │                  0 │           0 │
│ libmain.a               │      41140 │ 12118 │ 12109 │     9 │   188 │   188 │        0 │      28012 │  28012 │        822 │     822 │        0 │        0 │                  0 │           0 │
│ libwpa_supplicant.a     │      34159 │  1146 │  1146 │     0 │     0 │     0 │        0 │      32920 │  32920 │         93 │      93 │        0 │        0 │                  0 │           0 │
│ libc_nano.a             │      23493 │   988 │   724 │   264 │     0 │     0 │        0 │      21533 │  21533 │        972 │     972 │        0 │        0 │                  0 │           0 │
│ libfreertos.a           │      19887 │  3847 │   741 │  3106 │ 13782 │ 13782 │        0 │        643 │    643 │       1615 │    1615 │        0 │        0 │                  0 │           0 │
│ libesp_hw_support.a     │      19610 │   460 │   129 │   331 │ 11472 │ 11472 │        0 │       6526 │   6526 │       1096 │    1096 │        0 │       56 │                 24 │          32 │
│ libnvs_flash.a          │      12765 │    24 │    24 │     0 │     0 │     0 │        0 │      12594 │  12594 │        147 │     147 │        0 │        0 │                  0 │           0 │
│ libesp_system.a         │      11535 │   721 │   313 │   408 │  3819 │  3819 │        0 │       6387 │   6387 │        608 │     608 │        0 │        0 │                  0 │           0 │
│ libhal.a                │      10833 │  1327 │     8 │  1319 │  5324 │  5324 │        0 │       4069 │   4069 │        113 │     113 │        0 │        0 │                  0 │           0 │
│ libesp_driver_uart.a    │      10550 │   337 │    33 │   304 │     0 │     0 │        0 │       9899 │   9899 │        314 │     314 │        0 │        0 │                  0 │           0 │
│ libspi_flash.a          │       9807 │  1094 │    16 │  1078 │  7775 │  7775 │        0 │        688 │    688 │        250 │     250 │        0 │        0 │                  0 │           0 │
│ libheap.a               │       9805 │    12 │     8 │     4 │  5827 │  5827 │        0 │       2388 │   2388 │       1578 │    1578 │        0 │        0 │                  0 │           0 │
│ libespnow.a             │       4346 │    71 │    64 │     7 │     0 │     0 │        0 │       4275 │   4275 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libvfs.a                │       4335 │   236 │    44 │   192 │     0 │     0 │        0 │       3956 │   3956 │        143 │     143 │        0 │        0 │                  0 │           0 │
│ libesp_ringbuf.a        │       4331 │     0 │     0 │     0 │  3794 │  3794 │        0 │          0 │      0 │        537 │     537 │        0 │        0 │                  0 │           0 │
│ libesp_driver_gpio.a    │       3531 │    36 │     0 │    36 │   339 │   339 │        0 │       2968 │   2968 │        188 │     188 │        0 │        0 │                  0 │           0 │
│ libxtensa.a             │       3419 │  1044 │     0 │  1044 │  2240 │  1813 │      427 │         99 │     99 │         36 │      36 │        0 │        0 │                  0 │           0 │
│ libnewlib.a             │       3090 │   360 │   200 │   160 │  1370 │  1370 │        0 │       1251 │   1251 │        109 │     109 │        0 │        0 │                  0 │           0 │
│ libesp_wifi.a           │       2906 │   498 │    14 │   484 │   304 │   304 │        0 │       1308 │   1308 │        796 │     796 │        0 │        0 │                  0 │           0 │
│ libesp_timer.a          │       2862 │    56 │    24 │    32 │  1402 │  1402 │        0 │       1280 │   1280 │        124 │     124 │        0 │        0 │                  0 │           0 │
│ libesp_mm.a             │       2798 │   136 │   128 │     8 │   830 │   830 │        0 │       1667 │   1667 │        165 │     165 │        0 │        0 │                  0 │           0 │
│ libesp_pm.a             │       2504 │   155 │   127 │    28 │  1290 │  1290 │        0 │       1010 │   1010 │         49 │      49 │        0 │        0 │                  0 │           0 │
│ libbootloader_support.a │       1935 │     0 │     0 │     0 │  1818 │  1818 │        0 │         77 │     77 │         40 │      40 │        0 │        0 │                  0 │           0 │
│ libesp_common.a         │       1836 │     0 │     0 │     0 │     0 │     0 │        0 │         46 │     46 │       1790 │    1790 │        0 │        0 │                  0 │           0 │
│ libesp_partition.a      │       1631 │     8 │     8 │     0 │     0 │     0 │        0 │       1455 │   1455 │        168 │     168 │        0 │        0 │                  0 │           0 │
│ libesp_phy.a            │       1611 │    53 │    36 │    17 │   205 │   205 │        0 │       1140 │   1140 │        213 │     213 │        0 │        0 │                  0 │           0 │
│ liblog.a                │       1577 │   284 │   280 │     4 │   367 │   367 │        0 │        878 │    878 │         48 │      48 │        0 │        0 │                  0 │           0 │
│ libsoc.a                │       1526 │    40 │     0 │    40 │    37 │    37 │        0 │         41 │     41 │       1408 │    1408 │        0 │        0 │                  0 │           0 │
│ libstdc++.a             │       1477 │    21 │    17 │     4 │     0 │     0 │        0 │       1257 │   1257 │        199 │     199 │        0 │        0 │                  0 │           0 │
│ libesp_event.a          │       1463 │     4 │     4 │     0 │     0 │     0 │        0 │       1402 │   1402 │         57 │      57 │        0 │        0 │                  0 │           0 │
│ libesp_vfs_console.a    │        644 │    16 │    16 │     0 │     0 │     0 │        0 │        448 │    448 │        180 │     180 │        0 │        0 │                  0 │           0 │
│ libxt_hal.a             │        475 │     0 │     0 │     0 │   443 │   443 │        0 │          0 │      0 │         32 │      32 │        0 │        0 │                  0 │           0 │
│ libpthread.a            │        474 │    12 │     4 │     8 │     0 │     0 │        0 │        416 │    416 │         46 │      46 │        0 │        0 │                  0 │           0 │
│ librtc.a                │        463 │     0 │     0 │     0 │   463 │   463 │        0 │          0 │      0 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libesp_app_format.a     │        403 │    10 │    10 │     0 │     0 │     0 │        0 │        129 │    129 │        264 │       8 │      256 │        0 │                  0 │           0 │
│ libcore.a               │        284 │     9 │     9 │     0 │     0 │     0 │        0 │        275 │    275 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libesp_rom.a            │        251 │     0 │     0 │     0 │   219 │   219 │        0 │          0 │      0 │         32 │      32 │        0 │        0 │                  0 │           0 │
│ libesp_coex.a           │        245 │     0 │     0 │     0 │    98 │    98 │        0 │        147 │    147 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libesp_security.a       │        230 │     4 │     4 │     0 │     0 │     0 │        0 │        218 │    218 │          8 │       8 │        0 │        0 │                  0 │           0 │
│ libapp_update.a         │        172 │     4 │     4 │     0 │     0 │     0 │        0 │        138 │    138 │         30 │      30 │        0 │        0 │                  0 │           0 │
│ libesp_netif.a          │        165 │     9 │     8 │     1 │     0 │     0 │        0 │        156 │    156 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libgcc.a                │         89 │     0 │     0 │     0 │     0 │     0 │        0 │         89 │     89 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libcxx.a                │         50 │     4 │     4 │     0 │     0 │     0 │        0 │         46 │     46 │          0 │       0 │        0 │        0 │                  0 │           0 │
│ libnvs_sec_provider.a   │          5 │     0 │     0 │     0 │     0 │     0 │        0 │          5 │      5 │          0 │       0 │        0 │        0 │                  0 │           0 │
└─────────────────────────┴────────────┴───────┴───────┴───────┴───────┴───────┴──────────┴────────────┴────────┴────────────┴─────────┴──────────┴──────────┴────────────────────┴─────────────┘
```

**Reading this table:** `libmain.a` — **41,140 bytes** — is the *only* row that is
lattice-nodes application code (28,012 B flash code + 822 B flash rodata + 12,109 B
`.bss` + 9 B `.data`). Everything else is ESP-IDF/mbedTLS/WiFi framework pulled in
transitively. The largest non-application contributors:

- `libnet80211.a` (WiFi 802.11 MAC layer, pulled in because ESP-NOW rides on the WiFi
  stack even though this firmware never associates to an AP): 138,325 B — the single
  largest contributor in the whole image.
- `libpp.a` (WiFi PHY packet processing): 69,175 B
- `libmbedcrypto.a` (mbedTLS — ChaCha20/Poly1305/HKDF/SHA/Curve25519, per the Phase J
  crypto revert): 67,635 B
- `liblwip.a` (TCP/IP stack — mostly dead weight here since
  `CONFIG_LWIP_TCP_ENABLED=n`/`CONFIG_LWIP_UDP_ENABLED=n`, but still linked in via the
  WiFi/netif dependency chain): 65,290 B
- `libphy.a` (WiFi radio PHY calibration): 46,222 B
- `libefuse.a` (almost entirely `.rodata` lookup tables via
  `esp_efuse_utility.c.obj`, 44,508 B of its 44,852 B total): 44,852 B
- `libwpa_supplicant.a` (needed transitively for the WiFi/ESP-NOW crypto path even with
  no AP association): 34,159 B
- `libc_nano.a` (newlib-nano): 23,493 B
- `libfreertos.a`: 19,887 B

### Largest application object files (`idf.py size-files`, real captured data)

`firmware/main/src/**` (plus `main.cpp`) compiles to far more translation units than
fit usefully in a doc; the table below lists only the **16 largest** of them, filtered
from the full `idf.py size-files` run down to `firmware/main/src/**` + `main.cpp`
object files and then further cut to the biggest entries (columns: Total | DRAM
`.bss+.data` | Flash `.text` | Flash `.rodata`; all bytes). **This is not the complete
per-object-file breakdown** — smaller application object files are omitted, including
most of `adapter/` (only `Adapter.cpp.obj`/`SerialFraming.cpp.obj` made the cut),
`hardware/`, `persistence/eeprom/` (only `EepromCore.cpp.obj` made the cut), `error/`,
`logging/`, and `mesh/CompactMessage.cpp.obj` (unused per §5 regardless of size):

| Object file | Total | DRAM | Flash `.text` | Flash `.rodata` |
|---|---|---|---|---|
| `main.cpp.obj` | 15,332 | 12,073 (12,064 `.bss` / 9 `.data`) | 3,155 | 104 |
| `Mesh.cpp.obj` | 3,202 | 4 | 3,173 | 25 |
| `MeshMessenger.cpp.obj` | 2,367 | 0 | 2,329 | 38 |
| `RouteReportHandler.cpp.obj` | 1,355 | 0 | 1,355 | 0 |
| `SerialFraming.cpp.obj` | 1,111 | 0 | 823 | 288 |
| `MeshTransport.cpp.obj` | 997 | 4 | 839 (+148 IRAM) | 6 |
| `Enrollment.cpp.obj` | 884 | 0 | 852 | 32 |
| `MasterBeacon.cpp.obj` | 857 | 0 | 851 | 6 |
| `Adapter.cpp.obj` | 840 | 7 | 754 | 79 |
| `EepromCore.cpp.obj` | 819 | 8 | 811 | 0 |
| `PeerRegistry.cpp.obj` | 564 | 0 | 564 | 0 |
| `DownlinkRouter.cpp.obj` | 480 | 0 | 480 | 0 |
| `FrameAuthorizer.cpp.obj` | 319 | 0 | 319 | 0 |
| `PeerEnrollment.cpp.obj` | 292 | 0 | 292 | 0 |
| `UplinkRouter.cpp.obj` | 279 | 0 | 273 | 6 |
| `PendingRelayQueue.cpp.obj` | 120 | 0 | 120 | 0 |

`main.cpp.obj`'s **12,073 bytes of static DRAM** is the single largest "our code" RAM
contributor by far, dominated by `.bss` (12,064 B) — this is the two static FreeRTOS
task stacks declared in `main.cpp` (`mesh_task_stack[4096]` + `housekeeping_stack[4096]`
= 8,192 B) plus the global `Mesh mesh` object, `mesh_message transmissionMessage`, and
other file-scope globals declared there. This is also *why* the per-collaborator RAM
sizes in §5 below can't simply be read off `idf.py size-files`: every mesh collaborator
(`RouteTable`, `E2EKeyStore`, `PeerRegistry`, etc.) is a member subobject of the single
global `Mesh mesh` instance, so their combined static RAM is attributed entirely to
`main.cpp.obj`'s `.bss`, not split out per collaborator by the linker's per-object-file
report. Notice each collaborator's own `.cpp.obj` DRAM column above is 0 or a few bytes
(a static singleton pointer, e.g. `Mesh.cpp.obj`'s 4 bytes) — that's code-local statics,
not the collaborator's own data members.

Sum of the **16 rows shown above** (not the full application object-file list — see the
omissions noted before the table) is **29,818 B**. This is well short of `libmain.a`'s
reported 41,140 B, and the two numbers should not be read as "close": on top of the
smaller application object files this table deliberately excludes, the vendored/generated
nanopb serialization files (`pb_decode.c.obj` 3,210 B, `pb_encode.c.obj` 1,998 B,
`pb_common.c.obj` 687 B — third-party-generated code, not hand-written lattice-nodes
logic) also link into `libmain.a` but were excluded from the table above, and ordinary
per-object-file alignment/padding overhead disappears once objects are merged into the
archive. None of those three gap contributors were individually re-summed this pass, so
the ≈11,322 B gap (41,140 − 29,818) is explained qualitatively here, not reconciled
line-by-line.

## 4. RAM usage

From the same `idf.py size` run (see §3's summary table):

| Region | Used | Total | % | Free |
|---|---|---|---|---|
| IRAM (`.text` + `.vectors`) | 101,111 B | 131,072 B | **77.14%** | 29,961 B |
| DRAM (`.bss` + `.data`, i.e. static RAM) | 44,084 B | 180,736 B | **24.39%** | 136,652 B |
| RTC SLOW memory | 56 B | 8,192 B | 0.68% | 8,136 B |

DRAM breaks down as `.bss` 29,088 B (16.09%) + `.data` 14,996 B (8.3%).

**Important caveat (from the same research pass, not re-derived here):** the
136,652-byte DRAM "free" figure is *static-link-time* headroom, not free heap at
runtime. It does not yet account for FreeRTOS task-stack allocation beyond the two
static 4,096-byte stacks already counted in `main.cpp.obj` above, nor for the general
heap allocator's own overhead. True free-heap-at-runtime would require
`esp_get_free_heap_size()` on real, flashed hardware — out of scope for this
build-only measurement pass (no physical device was flashed to produce these numbers).

## 5. Fixed RAM allocations by mesh collaborator

The pre-Phase-B-split version of this document described a single "mesh object" with
one flat table of fixed-size members. That framing is gone: `firmware/main/src/mesh/`
is now ~16 distinct collaborator classes/namespaces (`RouteTable`, `E2EKeyStore`,
`PeerRegistry`, `ReplayCache`, `NeighborTable`, `PendingRelayQueue`, `MeshTransport`,
`MeshMessenger`, `Enrollment`, `MasterBeacon`, `UplinkRouter`, `DownlinkRouter`,
`FrameAuthorizer`, `RouteReportHandler`, `PeerEnrollment`, plus a handful of
header-only free-function namespaces), orchestrated by a thin `Mesh` class that owns
one instance of each. All of them live as member subobjects of the single global
`Mesh mesh` in `main.cpp` (see §3's note on why this defeats a simple per-object linker
breakdown).

The table below lists every collaborator confirmed (in this research pass) to own a
fixed-size RAM structure, together with what is and isn't independently confirmed for
each. **Per the task's accuracy bar, capacity bounds are only listed where a config
constant was actually read from the current `firmware/main/project_config.h` (or
equivalent) during this research pass; per-entry/total byte counts are listed only
where the research materials give a concrete figure. Everywhere else, this table says
"not confirmed this pass" rather than guessing.**

| Collaborator | Fixed structure | Confirmed capacity (source) | Per-entry / total bytes | Notes |
|---|---|---|---|---|
| `RouteTable` | node MAC → downlink source-route path | `LATTICE_ROUTE_TABLE_MAX = 16` (`project_config.h`, confirmed this pass) | **Not confirmed this pass — see caveat below** | Master-only; allocated conditionally via `Mesh::reevaluateRouteTable()`; leaves never pay this cost. |
| `E2EKeyStore` | per-peer derived key cache (`kUp`/`kDown`) | Role-conditional: `LATTICE_E2E_KEYCACHE_MAX = 10` (master), `LATTICE_E2E_KEYCACHE_MAX_LEAF = 2` (leaf) (`project_config.h`, confirmed this pass) | Not confirmed this pass | Resized via `setCapacity()` in `Mesh::reevaluateRouteTable()` on role change; round-robin eviction when full. |
| `PeerRegistry` | enrolled peer list (MAC + Curve25519 pubkey + last-seen) | `MAX_PEERS = 10` (confirmed via `EepromPeers.h`, this session's persistence-layer research; matches the RAM mirror's bound) | Not confirmed this pass | RAM mirror of the EEPROM/NVS-persisted peer list; leaf class, no other `mesh/` dependencies. |
| `ReplayCache` | per-origin replay high-water-mark (epoch, seq) | `LATTICE_REPLAY_MAX_ORIGINS = 12` (`project_config.h`, confirmed this pass) | Not confirmed this pass | LRU-evicted by origin when full. |
| `NeighborTable` | beacon-learned forwarding neighbors (MAC + hop-distance + freshness) | `LATTICE_NEIGHBOR_MAX = 8` (`project_config.h`, confirmed this pass) | Not confirmed this pass | Header-only, leaf class; deliberately never holds key material (trust split from `PeerRegistry`). |
| `DownlinkRouter`'s downlink-peer LRU | bounded auto-registered downlink-forwarding ESP-NOW peers | `LATTICE_DOWNLINK_PEER_MAX = 4` (`project_config.h`, confirmed this pass) | Not confirmed this pass | Bounds ESP-NOW peer-table exhaustion attacks; ownership moved here from a `Mesh`-level array since the Phase B split. |
| `PendingRelayQueue` | buffered `(mac, pubKey)` enrollment-relay entries | Capacity 4 (stated directly in this session's mesh architecture research; appears to be a local class constant, not a `project_config.h` "Tiger Style" global) | Not confirmed this pass | New collaborator since the pre-split baseline — extracted from `Enrollment` to fix a single-slot-latch bug that dropped concurrent enrollment requests. |
| `MeshTransport`'s RX ring (`recvQueue`) | ISR→task handoff ring buffer of `mesh_message` | Not confirmed this pass (the pre-split baseline cited `RECV_QUEUE_SIZE = 8`; not re-verified against current source in this research pass) | Not confirmed this pass | Ownership moved from a `Mesh`-level member into `MeshTransport` since the Phase B split; SPSC lock-free queue. |
| `OutboundSequenceState` | this node's outbound sequence counter + relay-dedup + epoch-rollback guard | N/A — small fixed scalar/flag state, not a bounded array | Small; not independently confirmed this pass | Owned by `Mesh`, threaded through `MeshMessenger`/`RouteReportHandler`/`MasterBeacon` for sequencing and nonce-reuse guarding. |

**Flagging a specific discrepancy rather than repeating it as fact:** this session's
mesh architecture research (`phaseD-research-mesh.md`) mentions RouteTable's size in
passing as "~2.25KB" ("leaves never pay its ~2.25KB"). That figure is not recomputed
in this pass — it is a carry-over from the pre-Phase-B-split baseline, which itself
assumed `LATTICE_ROUTE_TABLE_MAX = 32`. This research pass independently confirmed the
*current* bound is **16**, not 32 (a real, source-verified change — whether from
config tuning or unrelated drift, not established here). Since the bound that
produced "~2.25KB" is now confirmed stale, and no per-field byte size for
`RouteTable`'s entry struct was independently re-verified this pass, this document
does not restate "~2.25KB" as a current figure. A trustworthy number would need either
a fresh field-by-field read of `RouteTable.h`'s entry struct or a `sizeof()` print
compiled into a real build — neither was done here, per the instruction not to
estimate or reuse stale numbers.

**Collaborators confirmed to hold no fixed RAM allocation of their own** (pure
routing/security/dispatch logic, or free-function namespaces with no state):
`MeshMessenger` (aside from the constants/typedefs it defines), `MeshTransport` (aside
from `recvQueue` above), `UplinkRouter`, `DownlinkRouter` (aside from its LRU above),
`FrameAuthorizer`, `RouteReportHandler`, `MasterBeacon`, `PeerEnrollment`,
`E2EKeyLookup`, `RouteMac`, `MeshCrypto`, `E2ECrypto`. `Enrollment` additionally owns
this node's own fixed-size keypair and TOFU-learned master MAC(s), but their exact
combined byte total was not independently confirmed this pass either.

One more collaborator worth naming for completeness: `CompactMessage` — a ≤220-byte
in-RAM representation of `mesh_message`, `static_assert`-enforced — is **not wired
into any active code path**. It exists with a dedicated unit test but zero production
call sites (confirmed by grep across the whole `firmware/` tree during this session's
mesh research). It should not be described as something actively shrinking queue RAM
today.

## 6. Re-measuring after future changes

Exact command sequence used to produce every number in this document:

```bash
source ~/esp/esp-idf/export.sh   # or wherever ESP-IDF v5.5.1 is installed
cd firmware
idf.py build
idf.py size
idf.py size-components   # per-library/archive breakdown
idf.py size-files        # per-object-file breakdown (note: idf.py size --files does
                          # not exist on IDF 5.5.1 — size-files is the separate
                          # subcommand that replaces it)
```

Prerequisite: `firmware/main/config/master_pubkey_pin.h` must already exist (see §2)
or the build fails at compile time before you get anywhere near a size report.

**Re-run this after any major feature addition and update the numbers in this
document.** This doc went stale once already (the 2026-07-13 `arduino-cli` baseline
sat unmeasured through five feature phases before this rewrite); the fix going forward
is discipline, not tooling — there is currently no CI job that runs `idf.py build` +
`idf.py size` and fails/flags on this doc going out of sync, so keeping it current is
a manual step after any change that touches `firmware/main/src/mesh/`,
`firmware/main/project_config.h`'s bound constants, or the crypto/WiFi dependency set.
