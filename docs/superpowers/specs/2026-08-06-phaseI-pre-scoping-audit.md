# Phase I pre-scoping audit — package/native-facility swap candidates

**Date:** 2026-08-06
**Scope:** lattice-nodes firmware post-Phase-H2 (branch `main` at `5de9e9c`).
**Method:** 3 parallel Explore agents scanning `mesh/`, `adapter/`, `persistence/`, `error/`, `hardware/`, `app/`, `network/`, `logging/`, `config/`, `sdkconfig.defaults`, `CMakeLists.txt`, `idf_component.yml`.
**Purpose:** find items complementing existing Phase I plan (BB-JJ). All items below are NEW — not already in `2026-08-04-post-phaseG-audit-findings.md`.

## Findings (bucket KK-WW, priority order)

| Item | Impact | Location | Change | Effort |
|---|---|---|---|---|
| **KK** | ~10-20 KB flash | `sdkconfig.defaults` | Trim unused mbedtls ECP curves (SECP*/BP*) + `MBEDTLS_ECDSA_C=n`. Only CURVE25519 is used (verified: MeshCrypto/E2ECrypto/Enrollment call `mbedtls_ecdh_*` with `MBEDTLS_ECP_DP_CURVE25519` only, no ECDSA symbols anywhere). Same lever as prior AES/GCM/CCM trim. | S |
| **LL** | Several KB flash | `sdkconfig.defaults` | `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` for prod. Current LATTICE_LOG_NONE only folds app's own Logger; ESP-IDF internal `ESP_LOGI/W/E` in esp_wifi/esp_now/nvs_flash/mbedtls/driver still ship at Info verbosity with format strings in flash. | S |
| **MM** | Perf + reliability | `firmware/main/src/hardware/output/SevenSegDisplay.cpp:70-123` | TM1637 bitbang: `digitalWrite`/`digitalRead` per bit (~16 calls/byte × 4 bytes/frame) → `gpio_set_level`/`gpio_get_level`. Arduino digitalWrite is ~5-10× slower (pin-lookup-table + virtual dispatch); imprecise timing risks marginal ACK failures. | M |
| **NN** | ~20-30 KB flash | `sdkconfig.defaults` | `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`. Default is full newlib printf-scanf. Zero `%f`/`%lf`/`%g` outside nanopb binary encode (which never touches newlib). Verify no `%lld`/64-bit-int reliance first. | S |
| **OO** | Correctness + dedup | `Mesh.h:190,195-197` (Mesh.cpp:334-351,407-412), `Enrollment.h:59-78` (Enrollment.cpp:89-98,203-213) | Two hand-rolled SPSC ring buffers (recvQueue + pendingRelayQueue) both handle ESP-NOW-callback→loop() handoff → `xRingbufferCreateStatic` + `xRingbufferSendFromISR`/`Receive`. Consolidates duplicated bug surface. | M |
| **PP** | Perf | `firmware/main/src/hardware/input/Button.cpp:35` | `Button::isPressed()` — `digitalRead(_pin)` polled every loop for 2 buttons → `gpio_get_level(_pin)`. Register read vs pin-table lookup. | S |
| **QQ** | Perf + flash | `firmware/main/src/hardware/input/Pir.cpp:23-34` | `attachInterrupt(digitalPinToInterrupt(...))` → `gpio_isr_handler_add`/`gpio_isr_handler_remove` + `gpio_set_intr_type`. Removes Arduino ISR dispatch indirection. Needs `gpio_install_isr_service()` in setup. | M |
| **RR** | Flash | `Led.cpp:12`, `Button.cpp:12`, `GpioInput.cpp:12`, `GpioOutput.cpp:12`, `SevenSegDisplay.cpp:43-44` | 6+ scattered `pinMode()` at boot → single bundled `gpio_config_t` (one output-group, one input-group). | S |
| **SS** | Perf | `E2ECrypto.h:102-128` (buildNonce/buildAad), `RouteMac.h:30-48` (buildHopContext) | Manual byte-by-byte LE shift+mask+store for `epoch_num`/`seq_num`/`data_type` → `memcpy` from native LE field. Runs on every sealed/opened frame (every uplink + downlink). Verify no big-endian test-host build depends on manual form. | S |
| **TT** | Straggler | `Mesh.cpp:72,298,424`, `EepromManager.cpp:187` | Straggler `String(...) + esp_err_to_name(...)` heap-alloc sites → `LATTICE_LOGF`. Phase H2 caught ~35 of these but missed these 4. | S |
| **UU** | Flash | `EepromManager.cpp:40-48` | Hand-rolled bitwise CRC16-CCITT (poly 0x1021) → `esp_rom_crc16_le`/`esp_crc16_le` if wire/NVS-format compatible. Called once per boot on keypair load/save; code-size win, not perf. | S |
| **VV** | Perf (folds into DD) | `SerialAdapter.cpp:68-74` | Byte-at-a-time `Serial.available()`/`Serial.read()` every loop tick → `uart_read_bytes()` bulk or `UART_PATTERN_DET` ISR. Naturally folds into DD's uart_driver migration. Only worth separate item if DD implementer does naive 1:1 port. | S |
| **WW** | Latency | `Led.cpp:113-117` | `Led::blink()` uses blocking `delay(onTimeMs)`/`delay(offTimeMs)`. Blocks single Arduino loop task (mesh.loop/adapter.loop) for `times*(on+off)ms`. Low priority — arduino-esp32 delay internally yields via vTaskDelay; latency/design note, not raw cost. | M |

## Estimated cumulative wins (on top of BB-JJ)

- KK + LL + NN alone: **~30-70 KB flash** (sdkconfig-only, S effort — trivial risk).
- MM + PP + QQ + RR + SS: measurable perf + minor flash on hot GPIO/crypto paths.
- OO: single consolidated FreeRTOS ringbuf primitive for two subsystems.

## Non-findings (audited but nothing to change)

- `network/MacEq.h`, `mac_table.h`, `mem.h`, `hw_mac.h` — already product of Phase G/H2 dedup.
- No `std::function`/`std::vector`/`<algorithm>`/`std::unique_ptr` in `network/`, `logging/`, `config/` to swap.
- `Logger`'s hand-rolled `Serial.print` impl (vs `esp_log`) is INTENTIONAL: SerialAdapter uses raw Serial for binary framing; routing app logs through esp_log console (same UART) would corrupt the stream. LOG_NONE compile-time fold is the correct mitigation.
- No unused `idf_component.yml`/CMakeLists REQUIRES.
- No I2C/SPI, analogRead/PWM, OTA, or custom EEPROM-on-NVS to migrate.
- `CONFIG_COMPILER_STACK_CHECK`, `CONFIG_FREERTOS_QUEUE_REGISTRY_SIZE`, `CONFIG_HEAP_POISONING_*`, `CONFIG_ESP_COREDUMP_ENABLE` — already at minimal/off defaults per IDF v5.5.1.

## Recommendation

Fold **KK, LL, NN** (trivial sdkconfig flips) into a small pre-Phase-I "sdkconfig sweep" — probably ~30-70 KB flash for zero code churn. Keep BB-JJ scope intact; append **OO, MM, PP, QQ, RR, SS, TT** as Phase I task 2 or Phase I2. **UU, VV, WW** → deferred / opportunistic (fold into whichever task touches the same area).
