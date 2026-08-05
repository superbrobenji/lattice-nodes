# Post-Phase-G Code-Review Audit — Findings Ledger

**Status:** Reference document (not an implementation spec).
**Date:** 2026-08-04
**Method:** 4 parallel Explore-agent audits across mesh subsystem, non-mesh subsystems, cross-cutting patterns, heap+ESP-IDF leverage.
**Purpose:** capture every audit finding with rank, saving, effort, and disposition (Phase G / H / I / keep-as-is) so future sessions have full context.

## Bucket assignments

- **Phase G (expanded, current)** — items A-Q (17 no-cost / trivial-risk wins).
- **Phase H (post-G medium refactor)** — items R-AA (10 medium-scope refactors).
- **Phase I (native ESP-IDF leverage)** — items BB-JJ (9 large-scope Arduino→ESP-IDF migrations).

## Full findings table

### Phase G additions (A-Q — no-cost / trivial-risk)

| ID | File:line | Description | Saving | Effort |
|---|---|---|---|---|
| A | Multiple (`Mesh.cpp` `printMac`/`printMeshMessage`/`generateRandomMeshKey`/`meshKeyIsSet`; `Adapter.cpp` LED stub + RELAY enum + `LED_ADAPTER_DEFAULT_PIN`; `Error.h:88-97` `ERROR_ASSERT`/`ERROR_CHECK`; `MacAddress(const String&)` ctor) | Delete dead code | ~1-2 KB flash | trivial |
| B | `project_config.h:117`, `E2EKeyStore.h:73` | Role-split `LATTICE_E2E_KEYCACHE_MAX`: leaves need ≤2 (primary+secondary master), masters may need more. Mirror `reevaluateRouteTable` pattern from Phase B. | ~576 B RAM per leaf | low |
| C | `Enrollment.h:54` `PENDING_RELAY_QUEUE_SIZE` | Track `RECV_QUEUE_SIZE` 8→4 (its comment says "mirrors RECV_QUEUE"). | ~152 B RAM | trivial |
| D | `ReplayCache.h:11-17` `Entry` field order | Reorder `{mac[6], uint32 epoch, uint16 seq, uint32 lastSeenMs, bool used}` → `{uint32 epoch, uint32 lastSeenMs, uint16 seq, uint8 mac[6], bool used}` to save 4B/entry padding. | 48 B RAM | trivial |
| E | 7 sites (`Mesh.cpp:132,333,592,888,1069,1179,1342`, `Enrollment.cpp:72`) | Consolidate FF:FF broadcast MAC into one `constexpr` header. | ~50 B flash | trivial |
| F | `Adapter.cpp:50,74`, `SerialAdapter.cpp:32`, `SerialFraming.cpp:16,140` | Cache `esp_wifi_get_mac(WIFI_IF_STA, ...)` once at boot instead of per-RX-frame syscall. MAC doesn't change at runtime. | CPU + minor flash | low |
| G | `Enrollment.cpp:49-51`, `main.cpp:238,302,311` | Cache `isEnrolled()` — currently reads NVS via `_prefs.getBool` every call, called 2-3× per `loop()`. Set the cached bool in `init()`/`processJoinAck()`/`saveEnrolledFlag()`. | Per-loop NVS I/O eliminated | trivial |
| H | `Mesh.h:75` `externalRecvCallback`; `Enrollment.h:12` `RegisterPeerFn`, `EnrollmentRelayFn` | `std::function` → plain function-pointer typedefs. All existing binding sites are function pointers or captureless lambdas. | ~100 B RAM + ~1 KB flash | low |
| I | `hardware/input/GpioInput.h`, `hardware/output/GpioOutput.h`, plus `Pir.h`/`Button.h`/`Led.h`/`SevenSegDisplay.h` | Drop `virtual` on GpioInput/GpioOutput `init()` — never dispatched polymorphically. Keeps Adapter hierarchy virtual (that IS dispatched via `Adapter*`). | ~200-400 B flash + pointer/instance | low |
| J | `GpioInput.cpp:19-46`, `GpioOutput.cpp:19-42` | Replace pin-validation `switch` with `constexpr uint64_t VALID_MASK; return pin<64 && (MASK>>pin)&1;`. | ~150-300 B flash | trivial |
| K | `pir/PirAdapter.h:32` `_cooldownSeconds` → `constexpr`; collapse `_timerActive`/`_motionSent` into one enum | Reduce PIR adapter state redundancy. | ~6-10 B RAM/PIR node | trivial |
| L | `Adapter.h:34` `int _pin` → `uint8_t`; factory pin API `int` → `uint8_t` | ESP32 pins are 0-39; wide types waste padding. | ~3-4 B RAM/adapter | trivial |
| M | `SevenSegDisplay.cpp:180-244` `show()` and `showWithDP()` | Refactor into one `showInternal(value, leadingZeros, bool withDP)`; two 30-line copies differ only in `segs[3] |= 0x80`. | 200-350 B flash | low |
| N | `PirAdapter.cpp:68`, `SerialAdapter.cpp:20`, `SerialFraming.cpp:15` | Three copies of `static void readOwnMac(uint8_t[6])` → one helper in a small `hw_mac.h`. | ~150 B flash | trivial |
| O | `EepromManager.cpp` all `_prefs.put*` sites | `_persistOrEscalate` (from Phase A) used at only 5 of 18 sites. Route the other 13 through with `securityRelevant=false` so short-write failures don't silently discard. | ~150 B flash + robustness | low |
| P | `MeshCrypto.h`, `E2ECrypto.h` | Extend `MbedtlsGuard.h` with `EcpGroupCtx`, `MpiCtx`, `EcpPointCtx`, `ChaChaPolyCtx` — Phase E left these hand-managed even though they're the same UNIT_TEST-unwind hazard. | UNIT_TEST leak fix + 15 lines dedup | low |
| Q | ~60 sites across mesh + adapter | Canonical `bool lattice::mac::eq(const uint8_t*, const uint8_t*)` — currently two idioms (`memcmp(a,b,6)==0` and `MacAddress(a) == MacAddress(b)`), the latter is worse (extra memcpy). | ~1.5-2 KB flash | low-med |

### Phase H additions (R-AA — medium refactor)

| ID | File:line | Description | Saving | Effort |
|---|---|---|---|---|
| R | ~78 sites across mesh + adapter + hardware | `String`-concat log/format-arg elimination → `snprintf` into stack `char[]`. Heap churn currently fires even under Phase G's macro gating because arguments are constructed before the macro's ternary short-circuits. | Eliminates hot-path heap churn on every mesh frame | med |
| S | `app/DisplayManager.h:22-27` | `DisplayManager::tick` re-clocks TM1637 (6 blocking `writeByte`s, each up to 20ms ACK wait) every `loop()` tick once enrolled. Only refresh on value-change; keep 500ms toggle for pre-enroll. | ~100-200 ms/sec CPU reclaimed | low |
| T | `Button.cpp:26-35` (waitForHold `delay(10)`) | `Button::isPressed()` spins `delay(5)*2 = 10ms` blocking; ButtonHandler polls two buttons every loop → ~20ms/loop stalled. Switch to non-blocking last-poll timestamp + rolling debounce vote. | Removes ~20ms/loop stall; enables tickless idle | med |
| U | `Mesh.cpp:594,1070,1180,1343`, `Enrollment.cpp:73` | Fold 5 `esp_now_send(broadcastMac, ...)` sites into `sendBroadcast(const mesh_message&)` helper on Mesh. | ~100-200 B flash | low |
| V | `Adapter.cpp:41-93` vs `SerialAdapter.cpp:105-129,312-405` | OP_CONFIG_SET/OP_NODE_ID_SET/OP_HEALTH_REQ/OP_TX_POWER_SET dispatch table (or `handleControlOp(op, data, isTarget)` helper) — currently 150 lines duplicated between base `Adapter::onMeshData` and `SerialAdapter::onMeshDataImpl` (SerialAdapter handles them again in `handleCompleteFrame`). | ~1-2 KB flash | med |
| W | `PirAdapter.cpp:72-86` vs `SerialAdapter.cpp:24-54` | Health-frame builder + tick-interval into `Adapter` base — `sendNodeHealth`/`sendHealthReport` are the same shape (opcode + adapterType + 6B MAC + 4B LE uptime into `data[64]`). | ~300-500 B flash + 10B RAM | low |
| X | `Mesh.cpp:843,850` `processMasterBeacon` | Fold `neighbors.observe(...)` + `neighbors.minFreshDistance(millis())` into single pass — currently two full linear passes of the neighbor table on every beacon RX. Have `observe()` return the freshly-minimum distance, or a combined `observeAndMinDistance()`. | Halves neighbor-scan cost per beacon | low |
| Y | mesh subsystem (NeighborTable, RouteTable, E2EKeyStore, ReplayCache, PeerRegistry) | 5 classes reimplement the same "linear-scan-by-MAC + slot-allocate + `memcpy(entry.mac, mac, 6); valid=true;`" skeleton (~180 lines dup). Extract shared free helpers: `mac_find_index(entries, N, stride, offset, mac)` + `evict_oldest_by_ts(...)`. Prefer non-template helpers over `MacKeyedTable<Payload,N>` to avoid template bloat. | ~100-150 lines dedup + ~0.8-1.2 KB flash | med |
| Z | `Enrollment.cpp:158`, `E2EKeyStore.h:42`, `Mesh.cpp:1102` | `is_zero(const uint8_t*, size_t)` helper for 3 hand-rolled zero-check loops (6B and 32B). | tiny flash + clarity | trivial |
| AA | ~25 `getInstance()` sites (`EepromManager`, `Mesh`, `PirAdapter`, `ErrorCore`) | Singleton proliferation → free-function namespace holding file-static state. Each Meyers singleton emits `__cxa_guard_*` prologue (~40B + byte flag) per unique callsite. | ~0.5-1 KB flash + shorter callsites | med |

### Phase I additions (BB-JJ — native ESP-IDF leverage)

| ID | File:line | Description | Benefit | Effort |
|---|---|---|---|---|
| BB | `Mesh.cpp:8,294`, `Mesh.h:6` | Arduino `WiFi.h` → direct `esp_netif_init` + `esp_event_loop_create_default` + `esp_wifi_init(&cfg)` + `esp_wifi_set_storage(WIFI_STORAGE_RAM)` + `esp_wifi_set_mode(WIFI_MODE_STA)` + `esp_wifi_start()`. Currently `WiFi.h` pulls entire `WiFiGeneric/STA/AP/Scan/Client/Server`, TCP/IP-adapter shims — none needed for ESP-NOW-only use. | **~15-25 KB flash + several KB DRAM (LWIP contexts)** | med (well-documented ESP-IDF pattern) |
| CC | `persistence/EepromManager.{h,cpp}` | Arduino `Preferences` → direct `nvs_flash`/`nvs_open`/`nvs_get_*` API. Wrapper adds ~1-2 KB flash and hides `nvs_get_stats`, iterators, and the two-partition erase-log invariant needed for atomic epoch counter semantics. | ~1-2 KB flash + atomic epoch semantics | low |
| DD | `Logger.cpp`, `SerialAdapter.cpp:86-172`, `main.cpp:87,92,240-245` | Arduino `Serial` (`HardwareSerial` + `String::println` overloads) → `uart_driver_install(UART_NUM_0, rx, tx, 0, nullptr, 0)` + `uart_write_bytes` for framed serial-adapter TX. Arduino text logging → `esp_log.h` (`ESP_LOGI/W/E`) with built-in level gating + color. | Several KB flash + removes libc printf + Arduino String overloads | med (coordinate with Phase G/H log rewrites) |
| EE | `main.cpp:257-262,286-334` | Enable `CONFIG_PM_ENABLE=y` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`; configure `esp_pm_config_esp32_t{80, 240, true}` for light-sleep between beacon intervals. Add dedicated mesh drain task via `xTaskCreatePinnedToCoreStatic` pinned to core 1 + `xTaskNotifyGive` from RX callback → replace polled `while (recvQueueTail != recvQueueHead)` with block-until-notified. Frees `loop()` to sleep. | **~30-40% avg current for battery nodes** (order-of-magnitude power win) | med (verify ESP-NOW RX wakes CPU via WiFi task) |
| FF | 100+ sites (`Mesh.cpp`, `PeerRegistry.cpp:59,70,143`, `ButtonHandler.h`, `DisplayManager.h`, `NeighborTable`, `checkMasterTimeout`) | Arduino `millis()` (uint32, wraps ~49 days) → `esp_timer_get_time()` (int64 microseconds, wraps in 292 000 years). Introduce `static inline uint64_t millisNow()`, migrate `lastSeenMillis` fields to `uint64_t`. | Eliminates 49-day wrap bug class affecting `isPeerInRange`, `NeighborTable`, `checkMasterTimeout`, replay staleness | med (field-type audit) |
| GG | `E2ECrypto.h`, `MeshCrypto.h` | mbedtls ECDH+HKDF-SHA256 → libsodium `crypto_scalarmult_curve25519` + `crypto_kdf_derive_from_key`. `espressif__libsodium` already available as component-manager dep. Cache `EntropyCtx` + `CtrDrbgCtx` as static members of `E2EKeyStore` seeded once at boot; static-key ECDH doesn't need fresh DRBG entropy per call. | ~15-20 KB flash + smaller heap footprint | med-high (API surgery) |
| HH | `sdkconfig.defaults`, `main.cpp` boot | If staying with mbedtls (not swapping to libsodium): `CONFIG_MBEDTLS_MEMORY_BUFFER_ALLOC_C=y` + `mbedtls_memory_buffer_alloc_init(static_buf, 4*1024)` at boot. All mbedtls callocs are then confined to a static arena; can't fragment main heap. | Bounds + isolates crypto heap; makes fragmentation deterministic | low (config-only) |
| II | `sdkconfig.defaults` `CONFIG_ARDUINO_LOOP_STACK_SIZE=8192` | Halve to 4096 (or 4608 for safety). Deepest chain (`drainRecvQueue → processAdapterData → openPayload → mbedtls_chachapoly`) is ~1.5 KB. Measure high-water via `uxTaskGetStackHighWaterMark` first. | **4 KB permanent DRAM** | low |
| JJ | `PeerRegistry.cpp:75-128` | `loadFromEEPROM`/`saveToEEPROM` stack-allocate `MAX_PEERS * PEER_RECORD_SIZE = 380B` buffers just to iterate 10 records. Stream one record at a time from EepromManager to shrink transient stack. | 380 B stack (transient) | med (EepromManager API change) |

### Keep-as-is (looks refactorable, actually correct)

- **`std::unique_ptr<RouteTable> routes`** (Phase B) — allocation-only-on-master saves 2.25 KB on leaves; correct trade.
- **Round-robin `nextSlot` in `E2EKeyStore`** — LRU would cost RAM (timestamp/pointer per entry) for a "wrong eviction just re-derives" outcome.
- **`isMaster` + `currentMaster.distance` coexisting** — different semantics (declared role vs observed hops on non-masters).
- **Bounded `downlinkPeerLru` + hand-rolled MRU touch** — ESP-NOW peer-table eviction ordering matters; can't be shared cleanly with NeighborTable/RouteTable.
- **UNIT_TEST-only `throw FatalError` in `err::fatal`** — deliberate; on-device `[[noreturn]] while(true){}` remains exception-free.
- **Lock-free SPSC RX ring (`Mesh.cpp:357-374`)** — right pattern for WiFi-task→loop-task handoff; don't replace with FreeRTOS queue.
- **`esp_now_*` used directly** (no Arduino wrapper). Correct.
- **`esp_random()` at `Mesh.cpp:635,873`** — correct primitive.
- **`esp_wifi_set_ps(WIFI_PS_NONE)` at `main.cpp:229`** — correct for a receive-critical node; do NOT switch to modem-sleep (Phase I `PM_ENABLE` is light-sleep, which is different and compatible).
- **`IRAM_ATTR` on RX callback** — correct.
- **Nanopb (not libprotobuf)** — correct choice for embedded.
- **`makeErrorCode` in `ErrorCodes.h`** — display encoder for 7-seg; call sites pass digits directly, but keep it.

## Estimated cumulative savings

- **Phase G expansion (A-Q):** ~1.5 KB RAM + ~5-8 KB flash on top of already-planned wire+bounds+CompactMessage savings.
- **Phase H (R-AA):** ~1-2 KB flash + significant hot-path heap-churn elimination.
- **Phase I (BB-JJ):** ~35-45 KB flash + ~10 KB DRAM + 30-40% average current on battery nodes + 49-day wrap bug eliminated.

**Grand total (all 3 phases shipped):** ~40-55 KB flash reclaimed, ~12-15 KB DRAM reclaimed, dominant power win on battery deployments.

## Session provenance

Audit ran by 4 parallel Explore agents on 2026-08-04 during Phase G planning session. Findings verified by cross-referencing between agents (some items surfaced independently in ≥2 audits, e.g. MAC-compare consolidation appeared in mesh audit + cross-cutting audit). No agent output was accepted without cross-check.
