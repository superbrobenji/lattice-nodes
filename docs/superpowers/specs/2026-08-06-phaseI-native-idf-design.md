# Phase I — Native ESP-IDF leverage + full Arduino strip

**Date:** 2026-08-06
**Umbrella:** part of `2026-07-22-close-all-open-issues-design.md` (Phase I extension).
**Predecessors:** Phase G (`5de9e9c`), Phase H2 (`5de9e9c`).
**Feed audits:** `2026-08-04-post-phaseG-audit-findings.md` (BB-JJ), `2026-08-06-phaseI-pre-scoping-audit.md` (KK-WW, XX-AAA).

## Goal

Complete the Arduino→ESP-IDF migration started by Phase 0. Replace hand-rolled facilities with native ESP-IDF drivers, drop mbedtls entirely in favor of libsodium, enable tickless power management, and (final step) drop arduino-esp32 as a called component — its API surface reduced to nothing, `CONFIG_AUTOSTART_ARDUINO=y` removed, own `app_main()` bootstrap.

## Architecture

- **Firmware-only.** No wire changes. No hub/protocol changes.
- **Crypto primitives preserved.** ChaCha20-Poly1305 + X25519 + HKDF-SHA256 + HMAC-SHA256 stay — only the implementation swaps mbedtls→libsodium.
- **Sub-branch execution pattern (H2 proven).** Umbrella branch `docs/phaseI-native-idf` off `main`. One PR per task merged into umbrella. Final umbrella PR to `main` after full sweep + broad review.
- **Ordering.** Task 1 (sdkconfig sweep) first — fast risk-free validation of the ESP-IDF build path. Task 2 (GG libsodium) next while confidence fresh. Tasks 3-8 order flexible but 3+4 (WiFi + NVS raw) before 9 (PM), 3+4+5 before 10 (Arduino drop). Task 10 last — depends on every Arduino API being removed from the tree.

## Non-goals

- Not fixing e2e failures (293/293 already pass at branch tip; audit clarified prior "pre-existing" was env pollution).
- Not touching hub or protocol repos.
- Not retaining backwards compatibility with prior firmware images — reflash-required, same posture as prior phases.
- Not migrating Logger's UART strategy (intentional — SerialAdapter uses raw UART for binary framing; routing app logs through `esp_log` on same UART corrupts stream). DD's Logger scope narrows to SerialAdapter uart_driver only.
- Not adopting KK (mbedtls curve trim) — moot post-GG.

## Item-by-item disposition

### Sourced from BB-JJ (Phase G-era audit)

- **BB — Arduino WiFi.h → esp_wifi_*.** In Task 3. Extended by ZZ (below): explicit `esp_netif_init` + `esp_event_loop_create_default` + `esp_wifi_init` + `esp_wifi_set_mode(WIFI_MODE_STA)` + `esp_wifi_start()`. Removes WiFiGeneric/STA/AP/Scan/Client/Server. Est. ~15-25 KB flash + several KB DRAM.
- **CC — Arduino Preferences → nvs_flash direct.** In Task 4. Removes wrapper (~1-2 KB flash), unlocks `nvs_get_stats`, iterators, atomic-epoch guarantees.
- **DD — Arduino Serial + Logger → uart_driver + esp_log.h.** In Task 5, scoped narrower per non-goals: SerialAdapter migrates to `uart_driver_install` + `uart_write_bytes` + `uart_read_bytes` (folds VV). Logger stays hand-rolled with existing LOG_NONE compile-time fold. Several KB flash still won on SerialAdapter's Arduino Serial path.
- **EE — Tickless idle + PM + dedicated mesh task.** Task 9. `CONFIG_PM_ENABLE=y` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` + `esp_pm_config_esp32_t{min_freq=80, max_freq=240, light_sleep=true}` + `xTaskCreatePinnedToCoreStatic` for mesh drain (block on `xTaskNotifyGive` from ESP-NOW RX callback). Est. 30-40% avg current for battery nodes.
- **FF — millis() → esp_timer_get_time().** Task 6. 100+ sites. `uint32_t` millis fields → `uint64_t` micros. Eliminates 49-day wrap class.
- **GG — mbedtls → libsodium.** Task 2. All 6 mbedtls consumers have libsodium equivalents (`crypto_aead_chacha20poly1305_ietf_*`, `crypto_scalarmult_curve25519`, `randombytes_buf`, `crypto_kdf_hkdf_sha256`, `crypto_auth_hmacsha256`). Cache `EntropyCtx`/`CtrDrbgCtx` no longer needed — libsodium's `randombytes_buf` seeds once at boot via `esp_random`. Est. ~15-20 KB flash win from libsodium itself; full mbedtls drop pulls another ~25-40 KB (curves + MD dispatch + entropy).
- **HH — DROPPED.** Was alternative to GG. Not applicable post-libsodium.
- **II — ARDUINO_LOOP_STACK 8192→4096.** Task 6. Measure high-water via `uxTaskGetStackHighWaterMark` before flipping. 4 KB permanent DRAM.
- **JJ — PeerRegistry stream-per-record.** Task 6. `loadFromEEPROM`/`saveToEEPROM` currently stack-alloc 380 B; stream one at a time. 380 B transient stack.

### Sourced from KK-WW (pre-scoping audit)

- **KK — DROPPED.** Moot post-GG (mbedtls curve trim irrelevant when mbedtls is gone).
- **LL — CONFIG_LOG_DEFAULT_LEVEL_NONE=y.** Task 1. Folds ESP-IDF-internal log strings in esp_wifi/esp_now/nvs_flash/driver. Currently only app Logger folds.
- **MM — SevenSegDisplay TM1637 bitbang → gpio_set_level/gpio_get_level.** Task 7. Arduino digitalWrite ~5-10× slower per bit; 4 bytes × 16 calls per frame improved.
- **NN — CONFIG_LIBC_NEWLIB_NANO_FORMAT=y.** Task 1. Est. ~20-30 KB flash. Verify no `%lld`/64-bit-int format reliance first (post-FF `esp_timer_get_time` is int64 — format specifiers become `%lld` throughout — must audit and update before flipping).
- **OO — Ring buffers → xRingbufferCreateStatic.** Task 8. Consolidates Mesh::recvQueue + Enrollment::pendingRelayQueue.
- **PP — Button::isPressed digitalRead → gpio_get_level.** Task 7.
- **QQ — Pir::attachInterrupt → gpio_isr_handler_add.** Task 7. Needs `gpio_install_isr_service()` in setup.
- **RR — Scattered pinMode → bundled gpio_config_t.** Task 7. One config for output group, one for input.
- **SS — Manual byte-by-byte LE packing → memcpy.** Task 7. E2ECrypto/RouteMac hot path.
- **TT — 4 straggler String() → LATTICE_LOGF sites.** Task 7. Mesh.cpp:72,298,424 + EepromManager.cpp:187.
- **UU — CRC16-CCITT → esp_rom_crc16_le (opportunistic).** Fold into Task 4 if wire/NVS-format compatible; else defer.
- **VV — Byte-at-a-time Serial.read → uart_read_bytes bulk.** Folded into DD (Task 5).
- **WW — Led::blink blocking delay.** Fold into Task 7 as non-blocking state machine — required for EE PM to actually sleep.

### Sourced from XX-AAA (ESP-NOW direct + Arduino strip audit)

- **New sdkconfig flags (Task 1 additions):**
  - `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` + `_RX_ENABLED=n` — ESP-NOW frames small/discrete, no aggregation win.
  - `CONFIG_ESP_WIFI_NVS_ENABLED=n` — no WiFi creds persist.
  - `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n` + `CONFIG_ESP_WIFI_ENABLE_SAE_PK=n` — no AP association.
  - `CONFIG_LWIP_TCP_ENABLED=n` + `CONFIG_LWIP_UDP_ENABLED=n` — CONDITIONAL: verify build survives (arduino-esp32 `WiFi.mode()` may transitively pull LWIP netif init). If build fails, keep enabled and revisit after ZZ (which replaces `WiFi.mode()`).
- **XX — String elimination (7 sites).** Folds into Task 7. Logger signatures change (`const String&` → `const char*`); MacAddress::toString to fixed-buffer; Mesh.cpp:72/298/424 to snprintf via LATTICE_LOGF; EepromManager.cpp:187 same.
- **YY = Task 7 GPIO natives.** Same as MM+PP+QQ+RR — not a new item.
- **ZZ — WiFi.mode → raw esp_wifi_*/esp_netif_*.** Fold into Task 3 (extends BB). One file (`Mesh.h`/`Mesh.cpp`). Removes the last arduino-esp32 WiFi wrapper dependency.
- **AAA — Drop CONFIG_AUTOSTART_ARDUINO=y, write own app_main().** New Task 10. Depends on Tasks 1-9 all landing (every Arduino API removed first). Kills entire arduino-esp32-as-called-component surface. `arduino-esp32` may still be a build dependency for pulled-in headers/types briefly, but no function of it runs. Est. ~40-60 KB flash + several KB DRAM (Arduino main task stack + heap init). Highest total win; highest risk if API scrub is incomplete.

## Task decomposition

Ten tasks, each independently reviewable, each ends with a green PR against the umbrella branch.

### Task 1 — sdkconfig sweep (LL + new WiFi/LWIP flags; NN deferred to Task 6)
- Add flags to `firmware/sdkconfig.defaults`: `CONFIG_LOG_DEFAULT_LEVEL_NONE=y`, `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n`, `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n`, `CONFIG_ESP_WIFI_NVS_ENABLED=n`, `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n`, `CONFIG_ESP_WIFI_ENABLE_SAE_PK=n`. LWIP TCP/UDP flags CONDITIONAL — try flipping to `n`, keep on with comment if build fails (revisit post-Task 3).
- **NN (`LIBC_NEWLIB_NANO_FORMAT=y`) NOT in this task.** Deferred to Task 6 because Task 6's FF migration introduces 64-bit `%llu` printf specifiers that nano-format supports fine, but the pre-FF `%lu` for uint32 millis with nano-format is a risk-window: flipping nano-format before FF completes could break log formatting mid-transition. Land NN after FF's specifier audit.
- Rebuild firmware with `idf.py build` (local ESP-IDF now installed at `$HOME/esp/esp-idf`). Measure `idf.py size` before/after.
- Est. delta: ~10-30 KB flash. Zero source-code churn.

### Task 2 — libsodium swap (GG)
- Add `espressif/libsodium` to `idf_component.yml`.
- Rewrite `MeshCrypto.h`/`E2ECrypto.h`/`RouteMac.h`/`Enrollment.cpp` to libsodium APIs (see mapping table in Q1 answer to user).
- Delete `MbedtlsGuard.h`; replace with SodiumGuard or inline RAII (libsodium contexts are trivially destructible).
- Remove all `CONFIG_MBEDTLS_*` from `sdkconfig.defaults` (once no consumer).
- Verify `sodium_init()` called once at boot in main.
- Unit tests: mirror existing mbedtls KAT tests to libsodium (RFC 8439 ChaCha20-Poly1305 vector; X25519 spec vector). E2E: no changes expected in on-wire behavior — full 293/293 suite must pass unchanged.
- Est. delta: ~40-60 KB flash + smaller heap.

### Task 3 — Raw WiFi + ESP-NOW init (BB + ZZ)
- Remove `#include <WiFi.h>` from `Mesh.h`. Add explicit `esp_netif.h`, `esp_event.h`, `esp_wifi.h`, `esp_now.h` includes.
- In Mesh init: `esp_netif_init` → `esp_event_loop_create_default` → `esp_netif_create_default_wifi_sta` (if LWIP still linked; else use raw esp_netif_new) → `esp_wifi_init(&cfg)` (WIFI_INIT_CONFIG_DEFAULT) → `esp_wifi_set_mode(WIFI_MODE_STA)` → `esp_wifi_start()`.
- Re-test Task 1's LWIP flags after landing this: if the raw netif call skips LWIP entirely, `CONFIG_LWIP_TCP_ENABLED=n`/`_UDP_ENABLED=n` may now build.
- Est. delta: ~15-25 KB flash + several KB DRAM.

### Task 4 — nvs_flash direct (CC + optional UU)
- Rewrite `EepromManager` (post-Task-6-of-Phase-H2 already a namespace) around `nvs_open`/`nvs_get_blob`/`nvs_set_blob`/`nvs_commit`.
- Preserve existing key/blob layout (no NVS re-init needed on upgrade — same namespace, same keys).
- Opportunistic: swap CRC16-CCITT for `esp_rom_crc16_le` if wire-format-compatible (verify polynomial + reflection); else skip UU.
- Est. delta: ~1-2 KB flash.

### Task 5 — SerialAdapter uart_driver (DD + VV)
- SerialAdapter migrates to `uart_driver_install(UART_NUM_0, rx, tx, 0, nullptr, 0)` + `uart_write_bytes` for TX + `uart_read_bytes` for bulk RX. Consider `UART_PATTERN_DET` ISR for frame-boundary detection (optional; benchmark first).
- Logger UNCHANGED — its hand-rolled Serial.print path intentionally shares the UART with SerialAdapter's binary framing (routing through esp_log corrupts). LOG_NONE compile-time fold remains the correct production mitigation.
- Est. delta: ~2-4 KB flash on SerialAdapter path.

### Task 6 — Time + memory tightening (FF + JJ + II + NN)
- **FF:** 100+ `millis()` sites → `esp_timer_get_time() / 1000` for millis-compatible, or direct microseconds where the extra precision matters. Field types: `uint32_t lastSeenMillis` → `uint64_t lastSeenMs` (or `int64_t` — libsodium/esp APIs use int64). Comparisons remain wraparound-safe automatically since 64-bit doesn't wrap in practical lifetimes.
- **NN:** post-FF printf-specifier sweep — any `%u`/`%lu` now referring to 64-bit timestamps → `%llu`. Then set `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`. Est. ~20-30 KB flash.
- **JJ:** `PeerRegistry::loadFromEEPROM`/`saveToEEPROM` — stream one 38-byte record at a time via `nvs_get_blob` with offset (Task 4 dependency) instead of 380 B stack-alloc.
- **II:** measure `uxTaskGetStackHighWaterMark(NULL)` from loop() at various points; if headroom > 4 KB, flip `CONFIG_ARDUINO_LOOP_STACK_SIZE=4096`. Save 4 KB DRAM.
- Est. delta: ~20-30 KB flash + ~4 KB DRAM + 49-day wrap eliminated.

### Task 7 — GPIO natives + LE memcpy + String elimination + WW (MM+PP+QQ+RR + SS + TT + XX + WW)
- **MM:** SevenSegDisplay TM1637 bitbang — replace `digitalWrite` with `gpio_set_level`, `digitalRead` with `gpio_get_level`. Preserve `delayMicroseconds(3)` timing (TM1637 spec requirement).
- **PP:** `Button::isPressed()` uses `gpio_get_level` instead of `digitalRead`.
- **QQ:** Replace Arduino `attachInterrupt(digitalPinToInterrupt(pin), fn, RISING)` with `gpio_set_intr_type(pin, GPIO_INTR_POSEDGE)` + `gpio_isr_handler_add(pin, fn, arg)`. Requires one-time `gpio_install_isr_service(0)` in main setup.
- **RR:** Delete scattered `pinMode()` calls; add `gpio_config(&output_cfg)` + `gpio_config(&input_cfg)` at main boot, `pin_bit_mask` covering all pins in each direction group.
- **SS:** `E2ECrypto::buildNonce`/`buildAad` + `RouteMac::buildHopContext` — replace byte-by-byte LE shift+mask+store with `memcpy(dst, &field, sizeof(field))`. ESP32 is little-endian; wire output identical.
- **TT:** 4 straggler `String()` → `LATTICE_LOGF` sites.
- **XX:** Logger signature `const String&` → `const char*`; `MacAddress::toString()` returns pointer to fixed buffer (thread-local or callsite-provided); remaining String call-sites in mesh/persistence.
- **WW:** `Led::blink()` from blocking `delay()` to state-machine: `Led::update(now_ms)` called from main loop; internal `phase`/`nextFlipAt` state; caller uses `Led::pulse(times, on_ms, off_ms)` to arm.
- Est. delta: perf + minor flash. Sets up EE (Task 9) — non-blocking Led required so PM can actually sleep.

### Task 8 — Ring buffers → xRingbuffer (OO)
- Delete Mesh's `recvQueue[]` + head/tail/count members. Replace with `RingbufHandle_t recvQueue = xRingbufferCreateStatic(size, RINGBUF_TYPE_NOSPLIT, storage_bytes, &_recvQueueStruct)`.
- Delete Enrollment's `_pendingRelayQueue[]` + head/count members. Same treatment.
- ESP-NOW RX callback: `xRingbufferSendFromISR(recvQueue, &msg, sizeof(msg), &woken)` + `portYIELD_FROM_ISR(woken)`.
- Consumer: `xRingbufferReceive(recvQueue, &size, 0)` in `Mesh::loop()`.
- Behavior identical, backed by FreeRTOS-tested primitive.

### Task 9 — Tickless PM + dedicated mesh task (EE)
- `sdkconfig.defaults`: `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_PM_DFS_INIT_AUTO=y`.
- In main: `esp_pm_config_esp32_t cfg = {.max_freq_mhz=240, .min_freq_mhz=80, .light_sleep_enable=true}; esp_pm_configure(&cfg);`.
- Create dedicated mesh task via `xTaskCreatePinnedToCoreStatic` (static alloc, no heap). Task body: `xTaskNotifyWait` for RX signal from Task 8's ringbuf ISR callback (`xTaskNotifyFromISR`).
- Loop yielding: mesh loop no longer polled from main's `loop()`; main's `loop()` shrinks to housekeeping (adapter loop, health tick — themselves largely idle).
- Requires Tasks 3+7+8: PM needs proper wake sources (no busy loops, no `delay()` blocks, GPIO ISRs armed correctly).
- Est. delta: 30-40% avg current on battery nodes. Manual multimeter measurement is the acceptance gate.

### Task 10 — Drop CONFIG_AUTOSTART_ARDUINO, own app_main (AAA)
- Verify all Arduino API removed: `grep -rn "digital(Read|Write)\|pinMode\|attachInterrupt\|Serial\.\|WiFi\.\|String\|delay(\|millis()" firmware/main/src/` must return zero (except intentional retained comment references or the ClassImpl-hidden Logger internals).
- Remove `CONFIG_AUTOSTART_ARDUINO=y` from `sdkconfig.defaults`.
- Delete `firmware/main/main.cpp`'s `setup()` + `loop()`. Write `app_main()` that:
  - Initializes hardware (gpio_config from Task 7's bundle).
  - Initializes NVS (Task 4).
  - Initializes WiFi + ESP-NOW (Task 3).
  - Spawns mesh task (Task 9).
  - Spawns housekeeping task (button + display + health).
  - `app_main()` returns; FreeRTOS scheduler owns runtime.
- Remove `arduino-esp32` from `idf_component.yml` REQUIRES if nothing still depends on its headers (may still be pulled by nanopb's Arduino.h test-only paths — verify).
- Est. delta: ~40-60 KB flash + several KB DRAM.

## Testing & measurement

- **Per task:** full unit + e2e suite (293 tests) must pass. Any drop investigated before proceeding.
- **Cumulative measurement:** each task reports `idf.py size` before/after. Ledger records deltas. Target cumulative: ~100-150 KB flash + ~10-13 KB DRAM at Phase I completion vs main tip `5de9e9c`.
- **Task 2 (GG) crypto correctness:** mirror mbedtls tests to libsodium. RFC 8439 KAT + X25519 test vectors + HKDF vectors.
- **Task 6 (FF) time-wrap audit:** grep for `<` and `-` on millis-derived values; verify uint64 preserves semantics.
- **Task 9 (EE) current measurement:** multimeter probe acceptance criterion. 30-40% avg current reduction vs Task 8 baseline. If < 20%, investigate before Task 10.
- **Task 10 (AAA) Arduino scrub:** greps zero remaining Arduino API. Build binary size shrinks by ≥ 30 KB.

## Risks

- **Task 2 (GG):** libsodium's HKDF-SHA256 needs verify — `crypto_kdf_hkdf_sha256` added in libsodium 1.0.19; confirm `espressif/libsodium` port version. If missing, implement HKDF-SHA256 manually via libsodium's `crypto_auth_hmacsha256` (10 lines).
- **Task 3 (BB+ZZ):** may break Task 1's LWIP flags. Sequence Task 3 → re-test Task 1 flags.
- **Task 9 (EE):** wake sources must be complete. Any lingering polling loop (Button::isPressed if PP incomplete) defeats sleep. Task 7 is a hard prerequisite.
- **Task 10 (AAA):** biggest scope, biggest risk. Full test suite + manual boot-to-idle test required. Any missed Arduino API surface = boot failure. Do NOT dispatch Task 10 without a green scan.
- **Session limits:** 10 tasks large. Split across sessions expected. Ledger + per-task branches + report files preserve state.

## Success criteria

1. All 10 tasks landed, per-task PR merged into umbrella, umbrella PR merged to `main`.
2. 293/293 tests pass at umbrella tip.
3. Cumulative size delta ≥ 60 KB flash reduction, ≥ 6 KB DRAM reduction.
4. Multimeter-measured average current on battery leaf node reduced ≥ 25%.
5. `grep -rn "Arduino\.h\|WiFi\.\|digital(Read|Write)\|String\|delay(\|millis()" firmware/main/src/` returns only Logger-internal / intentional-comment matches.
6. `CONFIG_AUTOSTART_ARDUINO` no longer in `sdkconfig.defaults`.

## Gating

Post-Phase-H2 (already merged, `5de9e9c`). No cross-repo. No wire changes. Merge order within Phase I sequential per Task table (Task 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10). Non-sequential dependencies noted per task.
