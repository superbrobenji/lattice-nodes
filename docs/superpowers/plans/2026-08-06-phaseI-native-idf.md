# Phase I — Native ESP-IDF leverage + full Arduino strip Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate lattice-nodes firmware off arduino-esp32 API surface onto native ESP-IDF facilities. Drop mbedtls in favor of libsodium. Enable tickless power management. Culminates in removing `CONFIG_AUTOSTART_ARDUINO=y` with our own `app_main()`.

**Architecture:** Firmware-only, single-repo. Ten tasks executed in sequence via sub-branch pattern (H2 proven): umbrella branch `docs/phaseI-native-idf` off `main`, one PR per task merged into umbrella, final umbrella PR to `main`. No wire/protocol/hub changes. Crypto primitives preserved (ChaCha20-Poly1305 + X25519 + HKDF-SHA256 + HMAC-SHA256) — only implementation swaps.

**Tech Stack:** ESP-IDF v5.5.1, arduino-esp32 3.3.10 (being retired), libsodium (via `espressif/libsodium` component-manager package), FreeRTOS (native primitives), gtest + host mocks for unit + e2e.

## Global Constraints

- **No wire changes.** No mesh frame format, opcode, or field change. Verify via test suite behavior on unchanged wire bytes.
- **No hub or protocol changes.** All 10 tasks stay in `lattice-nodes` repo.
- **293/293 tests must pass** at end of every task (unit + e2e). Any drop investigated before proceeding.
- **BUILDS: always cap parallelism.** `--parallel 1` for CTest, `-j2` max for cmake/make. User's Mac OOMs under full-parallel builds.
- **ESP-IDF build required per task.** Local ESP-IDF installed at `$HOME/esp/esp-idf` (v5.5.1). Env setup:
  ```bash
  export IDF_PATH=$HOME/esp/esp-idf
  export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.9_env
  XTENSA=$(ls -d $HOME/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$XTENSA:$PATH
  cp -f firmware/main/config/master_pubkey_pin.h.example firmware/main/config/master_pubkey_pin.h  # local-only, gitignored
  cd firmware && python $IDF_PATH/tools/idf.py reconfigure && python $IDF_PATH/tools/idf.py build
  ```
  (Important: `firmware/main/config/master_pubkey_pin.h` is gitignored — the local placeholder MUST NOT ship shadowing `tests/mocks/master_pubkey_pin.h`. Delete before running host tests.)
- **Host test rule:** always `rm -f firmware/main/config/master_pubkey_pin.h` before `cd tests && cmake --build build -j2 && ctest --parallel 1`. Otherwise placeholder shadows test pin and e2e enrollment fails silently (verified failure mode from 2026-08-06 session).
- **Size measurement per task.** Report `.text` + `.rodata` + `.bss` + `.data` deltas via `idf.py size`. Ledger tracks cumulative.
- **Preserve `Logger`'s hand-rolled Serial-based impl** (intentional — same UART as SerialAdapter binary framing; `esp_log` on same UART would corrupt stream). LOG_NONE compile-time fold is the correct production mitigation.
- **clang-format 18** (CI uses apt's clang-format = 18). Local: `/opt/homebrew/opt/llvm@18/bin/clang-format -i --style=file <files>`.
- **PRs against umbrella branch** `docs/phaseI-native-idf`, NOT main. Umbrella → main only at Phase completion after broad review.

---

## File Structure

### Tasks 1-2 (sdkconfig + libsodium swap)
- **Create:** `firmware/main/src/mesh/SodiumGuard.h` — RAII holder for libsodium contexts (or delete MbedtlsGuard.h with no replacement if all libsodium contexts are POD).
- **Modify:** `firmware/sdkconfig.defaults` — Task 1 adds log+wifi+lwip flags; Task 2 removes all `CONFIG_MBEDTLS_*`.
- **Modify:** `firmware/main/idf_component.yml` — Task 2 adds `espressif/libsodium`.
- **Modify:** `firmware/main/src/mesh/E2ECrypto.h`, `MeshCrypto.h`, `RouteMac.h`, `Enrollment.cpp` — Task 2 rewrites all mbedtls calls.
- **Delete:** `firmware/main/src/mesh/MbedtlsGuard.h` — Task 2 removes.
- **Modify:** `tests/unit/test_e2e_crypto.cpp` — Task 2 mirrors mbedtls KAT tests to libsodium APIs.
- **Modify:** `tests/CMakeLists.txt` — Task 2 replaces `FetchContent_Declare(mbedtls ...)` with libsodium.

### Task 3 (raw WiFi + ESP-NOW init)
- **Modify:** `firmware/main/src/mesh/Mesh.h`, `Mesh.cpp` — replace `#include <WiFi.h>` + `WiFi.mode()` with explicit `esp_netif_init` + `esp_event_loop_create_default` + `esp_wifi_init` + `esp_wifi_set_mode(WIFI_MODE_STA)` + `esp_wifi_start()`.

### Task 4 (nvs_flash direct)
- **Modify:** `firmware/main/src/persistence/EepromManager.cpp`, `EepromManager.h` — replace all `Preferences.*` calls with `nvs_open`/`nvs_get_blob`/`nvs_set_blob`/`nvs_commit`. Keep same namespace strings + key names so on-flash layout unchanged.

### Task 5 (SerialAdapter uart_driver)
- **Modify:** `firmware/main/src/adapter/serial/SerialAdapter.cpp` — replace `Serial.available()`/`Serial.read()`/`Serial.write()` with `uart_read_bytes` / `uart_write_bytes` after one-time `uart_driver_install` in init.

### Task 6 (esp_timer + JJ + II + NN)
- **Modify:** all files under `firmware/main/src/` calling `millis()` — ~100 sites, replace with `esp_timer_get_time() / 1000ULL` (or direct microseconds where precision helps).
- **Modify:** field type migrations `uint32_t lastSeenMillis` → `uint64_t lastSeenMs` across `mesh/NeighborTable.h`, `mesh/PeerRegistry.h`, `mesh/Enrollment.h`, `mesh/RouteTable.h`, `mesh/ReplayCache.h`, `mesh/Mesh.h`, `adapter/Adapter.h`, `app/DisplayManager.h`, `hardware/input/Button.h`.
- **Modify:** `firmware/main/src/mesh/PeerRegistry.cpp` — `loadFromEEPROM`/`saveToEEPROM` stream-per-record via `nvs_get_blob` offset arg.
- **Modify:** `firmware/main/main.cpp` — add `uxTaskGetStackHighWaterMark` diagnostic block for II sizing.
- **Modify:** `firmware/sdkconfig.defaults` — flip `CONFIG_ARDUINO_LOOP_STACK_SIZE=4096` + `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` after specifier sweep.

### Task 7 (GPIO natives + LE memcpy + String elim + Led non-blocking)
- **Modify:** `firmware/main/src/hardware/output/SevenSegDisplay.cpp` — TM1637 bitbang uses `gpio_set_level` / `gpio_get_level`.
- **Modify:** `firmware/main/src/hardware/input/Button.cpp` — `isPressed()` uses `gpio_get_level`.
- **Modify:** `firmware/main/src/hardware/input/Pir.cpp` — `attachInterrupt`/`detachInterrupt` use `gpio_isr_handler_add` / `gpio_isr_handler_remove` + `gpio_set_intr_type`.
- **Modify:** `firmware/main/main.cpp` — add one-time `gpio_install_isr_service(0)` + bundled `gpio_config_t` for output group + input group.
- **Delete or thin:** individual `pinMode` calls in `Led.cpp`, `Button.cpp`, `GpioInput.cpp`, `GpioOutput.cpp`, `SevenSegDisplay.cpp`.
- **Modify:** `firmware/main/src/mesh/E2ECrypto.h` (buildNonce/buildAad ~L102-128), `firmware/main/src/mesh/RouteMac.h` (buildHopContext ~L30-48) — byte-by-byte LE store → `memcpy`.
- **Modify:** `firmware/main/src/mesh/Mesh.cpp:72,298,424` + `firmware/main/src/persistence/EepromManager.cpp:187` — 4 straggler `String()` → `LATTICE_LOGF`.
- **Modify:** `firmware/main/src/logging/Logger.{h,cpp}` — swap public signature `const String&` → `const char*`. Update all callers.
- **Modify:** `firmware/main/src/network/MacAddress.h` — `toString()` returns pointer to fixed-buffer (thread-local or caller-provided).
- **Modify:** `firmware/main/src/hardware/output/Led.{h,cpp}` — `blink()` from blocking `delay()` to `Led::pulse(times, on_ms, off_ms)` arming state machine; new `Led::update(now_ms)` called from main loop.

### Task 8 (xRingbuffer)
- **Modify:** `firmware/main/src/mesh/Mesh.h`, `Mesh.cpp` — replace `recvQueue[]` + head/tail/count with `RingbufHandle_t recvQueue` + static storage.
- **Modify:** `firmware/main/src/mesh/Enrollment.h`, `Enrollment.cpp` — same treatment for `_pendingRelayQueue`.

### Task 9 (Tickless PM + dedicated mesh task)
- **Modify:** `firmware/sdkconfig.defaults` — add `CONFIG_PM_ENABLE=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_PM_DFS_INIT_AUTO=y`.
- **Modify:** `firmware/main/main.cpp` — `esp_pm_configure(&cfg)` at boot + `xTaskCreatePinnedToCoreStatic` for mesh drain task.
- **Modify:** `firmware/main/src/mesh/Mesh.cpp` — RX-callback path signals task via `xTaskNotifyFromISR`; loop() body moves to dedicated task blocking on `xTaskNotifyWait`.

### Task 10 (drop CONFIG_AUTOSTART_ARDUINO, own app_main)
- **Modify:** `firmware/sdkconfig.defaults` — remove `CONFIG_AUTOSTART_ARDUINO=y`.
- **Modify:** `firmware/main/main.cpp` — delete `setup()` + `loop()`; write `app_main()` bootstrap.
- **Modify:** `firmware/main/idf_component.yml` — remove `arduino-esp32` from REQUIRES if no residual header dep survives.
- **Modify:** every remaining file still `#include <Arduino.h>` — replace with narrower ESP-IDF includes.

---

## Task 1 — sdkconfig sweep (LL + new WiFi/LWIP flags)

**Files:**
- Modify: `firmware/sdkconfig.defaults`

**Interfaces:**
- Consumes: nothing.
- Produces: reduced flash/heap footprint via disabled subsystems. No API surface.

**Baseline before change:** capture pre-Task-1 `idf.py size` for comparison.

- [ ] **Step 1: Baseline size measurement**

Env setup:
```bash
export IDF_PATH=$HOME/esp/esp-idf
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.9_env
XTENSA=$(ls -d $HOME/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
export PATH=$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$XTENSA:$PATH
cp -f firmware/main/config/master_pubkey_pin.h.example firmware/main/config/master_pubkey_pin.h
cd firmware
rm -rf build sdkconfig managed_components
python $IDF_PATH/tools/idf.py reconfigure
python $IDF_PATH/tools/idf.py build
python $IDF_PATH/tools/idf.py size > /tmp/phaseI-task1-baseline-size.txt
```
Save the size output as Task 1 baseline.

- [ ] **Step 2: Append new sdkconfig flags**

Add these lines to the end of `firmware/sdkconfig.defaults`:

```
# Phase I Task 1: log + WiFi trim (item LL + audit XX-AAA)
CONFIG_LOG_DEFAULT_LEVEL_NONE=y

# ESP-NOW is our only WiFi use: no AP association, no aggregation, no persisted creds
CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n
CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n
CONFIG_ESP_WIFI_ENABLE_SAE_PK=n

# LWIP: no sockets used; disable TCP + UDP if arduino-esp32 WiFi.mode allows it.
# NOTE: If Task 1 build fails with these, comment them out and leave a TODO to
# re-enable them at Task 3 (which replaces WiFi.mode with raw esp_netif_new).
CONFIG_LWIP_TCP_ENABLED=n
CONFIG_LWIP_UDP_ENABLED=n
```

- [ ] **Step 3: Try to build**

```bash
cd firmware
rm -rf build sdkconfig
python $IDF_PATH/tools/idf.py reconfigure
python $IDF_PATH/tools/idf.py build 2>&1 | tail -20
```

If build succeeds: continue to Step 4.

If build FAILS due to LWIP TCP/UDP flags: comment those two lines out with a note `# TODO Task 3 — re-enable post-esp_netif migration`; re-run reconfigure + build.

- [ ] **Step 4: Post-fix size measurement**

```bash
python $IDF_PATH/tools/idf.py size > /tmp/phaseI-task1-post-size.txt
diff /tmp/phaseI-task1-baseline-size.txt /tmp/phaseI-task1-post-size.txt
```

Record delta: `.text`, `.rodata`, `.bss`, `.data`, total. Expected: `.rodata` down ~500 B - 2 KB from log fold; `.text` down ~5-15 KB from disabled WiFi features. If LWIP off worked, add another ~15-25 KB.

- [ ] **Step 5: Host test run**

```bash
cd ..
rm -f firmware/main/config/master_pubkey_pin.h  # DO NOT SHIP placeholder shadowing test pin
cd tests
rm -rf build
cmake -B build . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --parallel 1
```
Expected: 293/293 pass.

- [ ] **Step 6: Commit**

```bash
git add firmware/sdkconfig.defaults
git commit -m "$(cat <<'EOF'
config(phaseI/task1): trim ESP-IDF logging + WiFi feature set (items LL + audit XX-AAA)

CONFIG_LOG_DEFAULT_LEVEL_NONE folds ESP-IDF internal ESP_LOGI/W/E in
esp_wifi/esp_now/nvs_flash/mbedtls (app Logger already folds via LATTICE_LOG_NONE).

ESP-NOW-only usage: no AP association, no aggregation, no persisted WiFi creds.

LWIP TCP/UDP off pending WiFi.mode replacement at Task 3.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
git push -u origin fix/phaseI-task1-sdkconfig-trim
```

- [ ] **Step 7: Open PR against umbrella**

```bash
gh pr create --base docs/phaseI-native-idf --title "config(phaseI/task1): sdkconfig log+wifi trim" --body "$(cat <<'EOF'
## Summary
- LL: CONFIG_LOG_DEFAULT_LEVEL_NONE
- Audit XX-AAA: AMPDU off, WiFi NVS off, WPA3/SAE off
- LWIP TCP/UDP conditional (Task 3 dependency)

Size delta: (paste diff)

## Test plan
- [x] 293/293 host tests pass
- [x] ESP-IDF build clean
- [x] Size measurement recorded

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Task 2 — libsodium swap (GG, full mbedtls drop)

**Files:**
- Modify: `firmware/main/idf_component.yml` (add libsodium, keep arduino-esp32 for now)
- Modify: `firmware/sdkconfig.defaults` (remove all `CONFIG_MBEDTLS_*` after all callers migrated)
- Modify: `firmware/main/src/mesh/E2ECrypto.h` (161 lines) — rewrite `initE2ECrypto`, `seal`, `open`, `deriveE2EKeys`
- Modify: `firmware/main/src/mesh/MeshCrypto.h` (74 lines) — rewrite `generateKeypair`, `sign*`, `verify*`
- Modify: `firmware/main/src/mesh/RouteMac.h` (67 lines) — rewrite `hmacSha256`
- Modify: `firmware/main/src/mesh/Enrollment.cpp` — replace ECDH calls
- Delete: `firmware/main/src/mesh/MbedtlsGuard.h` (100 lines)
- Modify: `firmware/main/main.cpp` — `sodium_init()` in setup
- Modify: `tests/CMakeLists.txt` — replace `FetchContent_Declare(mbedtls ...)` block with libsodium equivalent (or use libsodium via find_package if already system-installed)
- Modify: `tests/unit/test_e2e_crypto.cpp` — mirror KAT tests to libsodium APIs
- Test: `tests/unit/test_libsodium_kat.cpp` (new) — RFC 8439 ChaCha20-Poly1305 vector + X25519 spec vector + HKDF-SHA256 vector

**Interfaces:**
- Consumes: nothing (leaf swap).
- Produces (public API preserved): `E2ECrypto::seal(...)`, `E2ECrypto::open(...)`, `E2ECrypto::deriveE2EKeys(...)`, `MeshCrypto::generateKeypair(...)`, `RouteMac::hmacSha256(key, ctx, out)`. Same signatures. Same bytes on wire.

**Mapping table (memorize before starting):**

| mbedtls call | libsodium equivalent |
|---|---|
| `mbedtls_chachapoly_setkey` + `_encrypt_and_tag` | `crypto_aead_chacha20poly1305_ietf_encrypt` |
| `mbedtls_chachapoly_setkey` + `_auth_decrypt` | `crypto_aead_chacha20poly1305_ietf_decrypt` |
| `mbedtls_ecdh_setup(CURVE25519)` + `mbedtls_ecdh_calc_secret` | `crypto_scalarmult_curve25519` |
| `mbedtls_entropy_func` + `mbedtls_ctr_drbg_seed` + `_random` | `randombytes_buf` (seeded once via `sodium_init` from esp_random) |
| `mbedtls_hkdf(SHA256, ...)` | `crypto_kdf_hkdf_sha256_extract` + `crypto_kdf_hkdf_sha256_expand` (libsodium ≥ 1.0.19) |
| `mbedtls_md_hmac(SHA256, ...)` | `crypto_auth_hmacsha256` |
| `mbedtls_mpi_read_binary` / `mbedtls_mpi_lset` | not needed — libsodium X25519 takes raw 32-byte inputs |

- [ ] **Step 1: Verify libsodium version in component manager**

```bash
grep -A2 "libsodium" firmware/main/idf_component.yml || echo "not present yet"
```

Then check libsodium version available:
```bash
python $IDF_PATH/tools/idf.py add-dependency "espressif/libsodium" 2>&1 | tail -5
```

If HKDF is not available in the packaged version (unlikely — 1.0.19 released 2023), fall back to a manual HKDF-SHA256 (10 lines) built on `crypto_auth_hmacsha256`. Document choice in commit message.

- [ ] **Step 2: Baseline size**

Repeat Task 1 env-setup + build; save size output as Task 2 baseline.

- [ ] **Step 3: Migrate `E2ECrypto.h`**

Read `firmware/main/src/mesh/E2ECrypto.h` end-to-end first. Replace the struct's mbedtls context members with libsodium POD keys, then rewrite each method.

Example (seal — the hottest path):
```cpp
// BEFORE (mbedtls):
int ret = mbedtls_chachapoly_encrypt_and_tag(&chachapoly, plaintext_len,
                                             nonce, aad, aad_len,
                                             plaintext, ciphertext, tag);

// AFTER (libsodium):
unsigned long long clen = 0;
int ret = crypto_aead_chacha20poly1305_ietf_encrypt(
    ciphertext_and_tag, &clen,
    plaintext, plaintext_len,
    aad, aad_len,
    NULL,               // nsec — always NULL for chachapoly-ietf
    nonce, key);
// tag is appended: last 16 bytes of ciphertext_and_tag
// Return 0 = success, non-zero = failure
```

Preserve nonce construction (still 12-byte IETF nonce; layout unchanged — buildNonce output identical to before).

- [ ] **Step 4: Migrate `MeshCrypto.h`**

For `generateKeypair`:
```cpp
// BEFORE (mbedtls_ecdh_gen_public):
uint8_t priv[32], pub[32];
mbedtls_ecp_gen_privkey(...);
mbedtls_ecp_mul(...);

// AFTER:
uint8_t priv[32], pub[32];
randombytes_buf(priv, 32);
crypto_scalarmult_curve25519_base(pub, priv);  // pub = priv * BASE
```

- [ ] **Step 5: Migrate `RouteMac.h`**

```cpp
// BEFORE:
mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                key, key_len, ctx, ctx_len, out);

// AFTER:
crypto_auth_hmacsha256(out, ctx, ctx_len, key);  // key must be 32 bytes for _hmacsha256
```

If existing key isn't 32 B, use `crypto_auth_hmacsha256_state` init/update/final variant.

- [ ] **Step 6: Migrate `Enrollment.cpp` ECDH sites**

Replace ECDH calls with `crypto_scalarmult_curve25519(shared, priv, peer_pub)`. Verify shared-secret bytes match mbedtls output (should be identical — both compute raw X25519).

- [ ] **Step 7: Delete `MbedtlsGuard.h`**

```bash
git rm firmware/main/src/mesh/MbedtlsGuard.h
```
Remove all `#include "MbedtlsGuard.h"` sites. If any callers used its RAII contexts, migrate to plain POD keys (libsodium contexts don't need explicit destroy — but zero secrets before scope exit via `sodium_memzero`).

- [ ] **Step 8: Add sodium_init to boot**

In `firmware/main/main.cpp` setup (or `app_main` if Task 10 has landed by then — for now, setup):
```cpp
#include <sodium.h>
// ...
if (sodium_init() < 0) {
  lattice::err::fatal(...);  // sodium not initialized
}
```

- [ ] **Step 9: Add KAT tests**

Create `tests/unit/test_libsodium_kat.cpp`. Include:
- RFC 8439 §2.8.2 ChaCha20-Poly1305 KAT (input, key, nonce, aad, expected ciphertext+tag).
- X25519 test vector from RFC 7748.
- HKDF-SHA256 test vector from RFC 5869 Appendix A.1.
Each test asserts byte-exact output.

- [ ] **Step 10: Update `tests/CMakeLists.txt`**

Replace the `FetchContent_Declare(mbedtls ...)` block with libsodium. Options:
- Prefer `find_package(sodium REQUIRED)` if system libsodium works on macOS/Linux.
- Otherwise `FetchContent_Declare(libsodium GIT_REPOSITORY https://github.com/jedisct1/libsodium GIT_TAG 1.0.19)`.
Update all test targets that linked `mbedtls` to link `sodium` instead.

- [ ] **Step 11: Rebuild + host test**

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests
rm -rf build
cmake -B build . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --parallel 1
```
Expected: 293/293 pass. New `test_libsodium_kat` should add 3+ new tests → 296+ total.

- [ ] **Step 12: ESP-IDF build + size**

Restore pin file, rebuild firmware, capture size. Expected delta: `.text` down ~40-60 KB, `.rodata` down ~10-30 KB (mbedtls dispatch tables gone).

- [ ] **Step 13: Remove mbedtls sdkconfig entries**

After Step 11+12 pass, remove from `firmware/sdkconfig.defaults`:
- `CONFIG_MBEDTLS_CHACHA20_C`
- `CONFIG_MBEDTLS_POLY1305_C`
- `CONFIG_MBEDTLS_CHACHAPOLY_C`
- `CONFIG_MBEDTLS_HARDWARE_SHA`
- `CONFIG_MBEDTLS_AES_C`
- `CONFIG_MBEDTLS_GCM_C`
- `CONFIG_MBEDTLS_CCM_C`
- `CONFIG_MBEDTLS_HARDWARE_AES`
- `CONFIG_MBEDTLS_HKDF_C`
- `CONFIG_MBEDTLS_TLS_ENABLED`
Rebuild — confirm still passes.

- [ ] **Step 14: Commit + push + PR**

```bash
git add -A  # careful — verify with git status first
git commit -m "feat(phaseI/task2): swap mbedtls → libsodium (item GG, full mbedtls drop)"
git push -u origin feat/phaseI-task2-libsodium
gh pr create --base docs/phaseI-native-idf --title "feat(phaseI/task2): mbedtls → libsodium" --body "..."
```

---

## Task 3 — Raw WiFi + ESP-NOW init (BB + ZZ)

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h` — remove `#include <WiFi.h>`, add `esp_netif.h`, `esp_event.h`, `esp_wifi.h`, `esp_now.h`.
- Modify: `firmware/main/src/mesh/Mesh.cpp:11,269` — replace `#include <WiFi.h>` + `WiFi.mode(WIFI_STA)` with explicit init sequence.

**Interfaces:**
- Consumes: nothing.
- Produces: `Mesh::begin()` semantic preserved (WiFi radio in STA mode, ESP-NOW ready). All internal.

- [ ] **Step 1: Baseline size**

- [ ] **Step 2: Replace WiFi.mode**

In `Mesh.cpp`, replace `if (!WiFi.mode(WIFI_STA)) {` block:
```cpp
// Replace WiFi.mode(WIFI_STA) with raw ESP-IDF init.
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_start());
```
(esp_wifi_set_storage(WIFI_STORAGE_RAM) makes CONFIG_ESP_WIFI_NVS_ENABLED=n from Task 1 fully effective.)

- [ ] **Step 3: Remove WiFi.h includes**

`Mesh.h`: `#include <WiFi.h>` → `#include <esp_wifi.h>` + `#include <esp_netif.h>` + `#include <esp_event.h>` + `#include <esp_now.h>`.

- [ ] **Step 4: Build + test**

Host tests + ESP-IDF build. If Task 1 had LWIP TCP/UDP commented out, try re-enabling those `=n` flags now — likely works after WiFi.mode is gone.

- [ ] **Step 5: Commit + push + PR**

Commit message notes size delta + LWIP status.

---

## Task 4 — nvs_flash direct (CC + opportunistic UU)

**Files:**
- Modify: `firmware/main/src/persistence/EepromManager.cpp`, `EepromManager.h` — replace all `Preferences` API calls with `nvs_open`/`nvs_get_blob`/`nvs_set_blob`/`nvs_commit`/`nvs_erase_key`.
- Modify: `firmware/main/main.cpp` — `nvs_flash_init()` called at boot before `lattice::eeprom::init()`.

**Interfaces:**
- Consumes: nothing.
- Produces: `lattice::eeprom::init()`, `lattice::eeprom::loadNodeId()`, `lattice::eeprom::saveNodeId(uint8_t)`, ... all preserved (same signatures, same semantics).

**On-flash layout preservation:**
- Same NVS namespace strings ("lattice") + same key names ("nodeId", "meshKey", "adapterType", etc). No migration needed on device flash — same on-flash bytes.

- [ ] **Step 1: Baseline size + confirm current API surface**

```bash
grep -oE "_prefs\.[a-zA-Z]+" firmware/main/src/persistence/EepromManager.cpp | sort -u
```
Expect: `.begin`, `.end`, `.getBytes`, `.putBytes`, `.getUChar`, `.putUChar`, `.remove` — the Preferences call surface to migrate.

- [ ] **Step 2: Add `nvs_flash_init()` to boot**

In main.cpp setup:
```cpp
esp_err_t nvs_err = nvs_flash_init();
if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
  ESP_ERROR_CHECK(nvs_flash_erase());
  nvs_err = nvs_flash_init();
}
ESP_ERROR_CHECK(nvs_err);
```

- [ ] **Step 3: Migrate `EepromManager` per operation**

For each Preferences call, replace with nvs_flash equivalent. Example:
```cpp
// BEFORE:
_prefs.begin("lattice", false);
_prefs.putBytes("meshKey", key, MESH_KEY_SIZE);
_prefs.end();

// AFTER:
nvs_handle_t h;
esp_err_t err = nvs_open("lattice", NVS_READWRITE, &h);
if (err == ESP_OK) {
  err = nvs_set_blob(h, "meshKey", key, MESH_KEY_SIZE);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
}
```

Preserve `_persistOrEscalate` wrapper semantic (from Phase A) — check every nvs return, escalate on failure.

- [ ] **Step 4: Opportunistic UU — CRC16 swap**

If `EepromManager.cpp:40-48` CRC16-CCITT (poly 0x1021) is called on the keypair blob:
```cpp
// BEFORE: hand-rolled bitwise loop
uint16_t crc = 0xFFFF;
for (size_t i = 0; i < len; i++) { ... }

// AFTER (only if same polynomial + reflection):
#include "esp_rom_crc.h"
uint16_t crc = esp_rom_crc16_le(0xFFFF, data, len);
```
Verify: `esp_rom_crc16_le` is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xor). If matches, swap. If not, skip UU and leave a comment.

- [ ] **Step 5: Build + test**

Host test build against firmware/main/src/ with `-DUNIT_TEST` — nvs_flash mock needs to exist in `tests/mocks/`. Check `tests/mocks/Preferences.cpp` — if it mocks Preferences directly, may need matching `nvs_flash.h` mock. Add mock if missing.

- [ ] **Step 6: ESP-IDF build + size**

- [ ] **Step 7: Commit + push + PR**

---

## Task 5 — SerialAdapter uart_driver (DD + VV)

**Files:**
- Modify: `firmware/main/src/adapter/serial/SerialAdapter.cpp` — replace `Serial.available()`/`Serial.read()`/`Serial.write()` with `uart_read_bytes`/`uart_write_bytes`.
- Modify: `firmware/main/src/adapter/serial/SerialAdapter.h` — add `uart_num` config.
- Modify: `firmware/main/main.cpp` — one-time `uart_driver_install(UART_NUM_0, ...)` before `SerialAdapter::init`.

**Interfaces:**
- Consumes: nothing.
- Produces: `SerialAdapter::loop()`, `SerialAdapter::sendBytes(...)` preserved.

**Critical:** Logger STAYS on hand-rolled Serial-based path (per non-goals). Both Logger and SerialAdapter share UART_NUM_0. Logger's writes must remain compatible — Arduino Serial.print writes to same UART driver, so if uart_driver_install is done once at boot, Logger's writes route through the same driver. Verify no double-init.

- [ ] **Step 1: Baseline size + verify Logger UART sharing**

Read `firmware/main/src/logging/Logger.cpp` — confirm it calls `Serial.print` (arduino wrapper). After `uart_driver_install(UART_NUM_0, ...)` in main setup, arduino's Serial internally uses the same driver — no conflict. Document this assumption.

- [ ] **Step 2: Install UART driver in boot**

In main.cpp setup, before Logger or SerialAdapter init:
```cpp
uart_config_t uart_cfg = {
  .baud_rate = 115200,  // match existing Serial.begin
  .data_bits = UART_DATA_8_BITS,
  .parity = UART_PARITY_DISABLE,
  .stop_bits = UART_STOP_BITS_1,
  .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  .source_clk = UART_SCLK_DEFAULT,
};
ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));  // RX 1024, TX unbuffered
ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_cfg));
```

- [ ] **Step 3: Migrate SerialAdapter reads**

Replace byte-at-a-time loop:
```cpp
// BEFORE:
while (Serial.available()) {
  uint8_t b = Serial.read();
  processByte(b);
}

// AFTER:
uint8_t buf[64];
int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), 0);  // 0 = non-blocking
for (int i = 0; i < n; i++) processByte(buf[i]);
```

- [ ] **Step 4: Migrate SerialAdapter writes**

```cpp
// BEFORE: Serial.write(byte)
// AFTER:  uart_write_bytes(UART_NUM_0, &byte, 1);

// BEFORE: Serial.write(buf, len)
// AFTER:  uart_write_bytes(UART_NUM_0, buf, len);
```

- [ ] **Step 5: Build + host test + ESP-IDF + size**

- [ ] **Step 6: Commit + push + PR**

---

## Task 6 — esp_timer + PeerRegistry stream + stack shrink + nano-format (FF + JJ + II + NN)

**Files:**
- Modify: ~30-40 files under `firmware/main/src/` calling `millis()`.
- Modify: ~10 header files with `uint32_t last*Millis` / `uint32_t last*Ms` fields → `uint64_t`.
- Modify: `firmware/main/src/mesh/PeerRegistry.cpp` — JJ stream-per-record load/save.
- Modify: `firmware/main/main.cpp` — II stack high-water diagnostic + stack shrink.
- Modify: `firmware/sdkconfig.defaults` — `CONFIG_ARDUINO_LOOP_STACK_SIZE=4096`, `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`.

**Interfaces:**
- Consumes: `nvs_get_blob` with offset (Task 4).
- Produces: no public API change. All-internal: field type change from uint32→uint64.

- [ ] **Step 1: Baseline + FF site inventory**

```bash
grep -rn "millis()" firmware/main/src/ --include="*.cpp" --include="*.h" | wc -l
grep -rn "uint32_t.*[Mm]illis\|uint32_t.*_ms\b" firmware/main/src/ | head -30
```

- [ ] **Step 2: FF migration — replace all `millis()` with `esp_timer_get_time() / 1000`**

Sweep pattern: `s/millis()/(esp_timer_get_time() \/ 1000ULL)/g` — but preserve type at the LHS. All `uint32_t now = millis();` → `uint64_t now_ms = esp_timer_get_time() / 1000ULL;`.

For fields, change types in the header files enumerated above. Cascade through comparisons — most `(now - last_ms) > THRESHOLD` compare 64-bit signed/unsigned correctly since no wrap.

- [ ] **Step 3: FF printf specifier sweep**

```bash
grep -rn "%u\|%lu\|%d\|%ld" firmware/main/src/ --include="*.cpp" --include="*.h" | grep -i "millis\|_ms\|last[SU]een\|now" | head -20
```
Any that reference migrated 64-bit fields → change specifier to `%llu` / `%lld`.

- [ ] **Step 4: FF host test + build**

Host tests must still pass — mock `esp_timer_get_time()` in `tests/mocks/time_mock.cpp` if not already. Should be similar to existing `millis` mock.

- [ ] **Step 5: JJ stream-per-record PeerRegistry**

In `PeerRegistry::loadFromEEPROM`:
```cpp
// BEFORE: single 380-byte stack blob
uint8_t blob[PEER_LIST_SIZE];
_prefs.getBytes("peers", blob, sizeof(blob));
for (int i = 0; i < NUM_PEERS; i++) memcpy(&peers[i], blob + i*sizeof(PeerInfo), sizeof(PeerInfo));

// AFTER: per-record via nvs_get_blob offset OR per-key naming
char key[16];
for (int i = 0; i < NUM_PEERS; i++) {
  snprintf(key, sizeof(key), "peer%d", i);
  size_t sz = sizeof(PeerInfo);
  err = nvs_get_blob(h, key, &peers[i], &sz);
  if (err == ESP_ERR_NVS_NOT_FOUND) memset(&peers[i], 0, sizeof(PeerInfo));
}
```

Same for save. Note: this migrates on-flash key layout ("peers" blob → "peer0"..."peer9" keys). REQUIRES device reflash — no backcompat per project posture.

- [ ] **Step 6: II stack high-water measure**

Add to main.cpp loop() (temporary diag):
```cpp
static uint32_t last_hw_check = 0;
if (esp_timer_get_time() / 1000ULL - last_hw_check > 5000) {
  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGI("STACK", "Arduino loop task high-water: %u bytes free", hw * sizeof(StackType_t));
  last_hw_check = esp_timer_get_time() / 1000ULL;
}
```

Flash + run for ~1 min covering enrollment + broadcast + relay. Read log — if high-water > 4096 B free, safe to flip stack to 4096.

- [ ] **Step 7: II flip stack size**

If Step 6 shows headroom > 4 KB:
```
CONFIG_ARDUINO_LOOP_STACK_SIZE=4096
```
Rebuild + retest. Remove the diagnostic block.

- [ ] **Step 8: NN nano-format flip**

After Step 3's specifier sweep, add:
```
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
```
Rebuild. Any `%llu`/`%lld` failing = missed a specifier in Step 3.

- [ ] **Step 9: Size measurement + commit + push + PR**

Expected: `.text` down ~20-30 KB (nano-format) + `.bss` down 4 KB (stack shrink). PeerRegistry stack down 380 B (transient).

---

## Task 7 — GPIO natives + LE memcpy + String elim + Led non-blocking

**Files:**
- Modify: `firmware/main/src/hardware/output/SevenSegDisplay.cpp` (MM)
- Modify: `firmware/main/src/hardware/input/Button.cpp` (PP)
- Modify: `firmware/main/src/hardware/input/Pir.cpp` (QQ)
- Modify: `firmware/main/main.cpp` (QQ: gpio_install_isr_service; RR: bundled gpio_config)
- Modify/thin: `firmware/main/src/hardware/output/Led.cpp`, `hardware/input/Button.cpp`, `hardware/input/GpioInput.cpp`, `hardware/output/GpioOutput.cpp` (RR: remove per-init pinMode)
- Modify: `firmware/main/src/mesh/E2ECrypto.h:102-128`, `firmware/main/src/mesh/RouteMac.h:30-48` (SS)
- Modify: `firmware/main/src/mesh/Mesh.cpp:72,298,424`, `firmware/main/src/persistence/EepromManager.cpp:187` (TT)
- Modify: `firmware/main/src/logging/Logger.h`, `Logger.cpp` (XX)
- Modify: `firmware/main/src/network/MacAddress.h` (XX)
- Modify: `firmware/main/src/hardware/output/Led.h`, `Led.cpp` (WW)

**Interfaces:**
- Consumes: nothing.
- Produces: `Logger::log`, `Logger::logln` signatures change: `const String&` → `const char*` — CROSS-CUTTING change; every caller in codebase must migrate.

**Signature change note (XX):** `Logger::log(const char* tag, const char* msg, LogLevel level)`. Callers passing `String` must call `.c_str()` OR (better) use `LATTICE_LOGF(tag, level, "%s", str.c_str())`.

- [ ] **Step 1: Baseline size**

- [ ] **Step 2: MM — SevenSegDisplay `digitalWrite`/`digitalRead` → `gpio_set_level`/`gpio_get_level`**

In `SevenSegDisplay.cpp`, replace call sites. `digitalWrite(pin, HIGH)` → `gpio_set_level(pin, 1)`. `digitalRead(pin)` → `gpio_get_level(pin)`. Preserve `delayMicroseconds(3)` timing (TM1637 spec).

- [ ] **Step 3: PP — Button `isPressed()` → gpio_get_level**

- [ ] **Step 4: QQ — Pir attach/detach interrupt → gpio_isr_handler_add/remove**

Replace `attachInterrupt(digitalPinToInterrupt(pin), fn, RISING)`:
```cpp
gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
gpio_isr_handler_add(pin, fn, arg);
gpio_intr_enable(pin);
```
Detach:
```cpp
gpio_intr_disable(pin);
gpio_isr_handler_remove(pin);
```

- [ ] **Step 5: QQ+RR — main.cpp setup: gpio_install_isr_service + bundled gpio_config**

In `main.cpp`:
```cpp
ESP_ERROR_CHECK(gpio_install_isr_service(0));

gpio_config_t out_cfg = {
  .pin_bit_mask = (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED) | (1ULL << PIN_TM1637_CLK) | (1ULL << PIN_TM1637_DIO),
  .mode = GPIO_MODE_OUTPUT,
  .pull_up_en = GPIO_PULLUP_DISABLE,
  .pull_down_en = GPIO_PULLDOWN_DISABLE,
  .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&out_cfg);

gpio_config_t in_cfg = {
  .pin_bit_mask = (1ULL << PIN_CONFIG_BUTTON) | (1ULL << PIN_RESET_BUTTON) | (1ULL << PIN_PIR),
  .mode = GPIO_MODE_INPUT,
  .pull_up_en = GPIO_PULLUP_ENABLE,  // match existing pinMode(INPUT_PULLUP)
  .pull_down_en = GPIO_PULLDOWN_DISABLE,
  .intr_type = GPIO_INTR_DISABLE,     // Pir sets its own after
};
gpio_config(&in_cfg);
```

Then delete per-init `pinMode()` calls in `Led.cpp`, `Button.cpp`, `GpioInput.cpp`, `GpioOutput.cpp`, `SevenSegDisplay.cpp`.

- [ ] **Step 6: SS — E2ECrypto + RouteMac LE packing → memcpy**

In `buildNonce`, replace byte-by-byte:
```cpp
// BEFORE:
out[0] = (uint8_t)(epoch_num & 0xFF);
out[1] = (uint8_t)((epoch_num >> 8) & 0xFF);
out[2] = (uint8_t)((epoch_num >> 16) & 0xFF);
out[3] = (uint8_t)((epoch_num >> 24) & 0xFF);
// (similar for seq_num, data_type)

// AFTER (ESP32 is LE — verified via static_assert if paranoid):
memcpy(out + 0, &epoch_num, sizeof(epoch_num));  // 4 bytes
memcpy(out + 4, &seq_num, sizeof(seq_num));      // 4 bytes
// data_type 2 bytes:
uint16_t dt16 = (uint16_t)data_type;
memcpy(out + 8, &dt16, sizeof(dt16));
```
Verify wire output byte-for-byte identical via test.

- [ ] **Step 7: TT — 4 straggler String() sites**

`Mesh.cpp:72,298,424` + `EepromManager.cpp:187`:
```cpp
// BEFORE:
LATTICE_LOGLN("MESH", String("Broadcast send failed: ") + esp_err_to_name(err), LogLevel::LOG_WARN);
// AFTER:
LATTICE_LOGF("MESH", LogLevel::LOG_WARN, "Broadcast send failed: %s", esp_err_to_name(err));
```

- [ ] **Step 8: XX — Logger signature change + caller migration**

Change `Logger.h`:
```cpp
static void logln(const char* tag, const char* msg, LogLevel level = LogLevel::LOG_INFO);
static void log(const char* tag, const char* msg, LogLevel level = LogLevel::LOG_INFO);
```
And `Logger.cpp`. Every caller passing `String` must call `.c_str()` — OR (preferred) migrate to `LATTICE_LOGF`. Grep + fix:
```bash
grep -rn "LATTICE_LOGLN\|Logger::log" firmware/main/src/ | grep "String\|+ \"\|(\".*)\.c_str"
```

- [ ] **Step 9: XX — MacAddress::toString → fixed buffer**

```cpp
// Header:
class MacAddress {
public:
  // Writes NUL-terminated "aa:bb:cc:dd:ee:ff" into out (18 bytes required).
  void toString(char out[18]) const;
};
```
Fix all callers of the old `String toString()`.

- [ ] **Step 10: WW — Led non-blocking blink**

Rewrite `Led::blink(times, on_ms, off_ms)` from blocking delay-loop to state-machine:
```cpp
class Led {
public:
  void pulse(uint8_t times, uint32_t on_ms, uint32_t off_ms);
  void update(uint64_t now_ms);  // call from main loop
private:
  uint8_t _remaining = 0;
  bool _on_phase = false;
  uint64_t _next_flip_ms = 0;
  uint32_t _on_ms = 0, _off_ms = 0;
};
```
Main loop calls `led.update(now_ms)`.

Fix all `Led::blink(...)` callers to use `.pulse(...)` + rely on loop's `.update()`.

- [ ] **Step 11: Build + host test + ESP-IDF + size**

Sub-step: format via `clang-format 18` before commit.

- [ ] **Step 12: Commit + push + PR**

---

## Task 8 — Ring buffers → xRingbufferCreateStatic (OO)

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h`, `Mesh.cpp` — replace `recvQueue[]` + head/tail with `RingbufHandle_t`.
- Modify: `firmware/main/src/mesh/Enrollment.h`, `Enrollment.cpp` — same for `_pendingRelayQueue`.

**Interfaces:**
- Consumes: nothing.
- Produces: no public API change.

- [ ] **Step 1: Baseline size**

- [ ] **Step 2: Mesh::recvQueue → static ringbuf**

In `Mesh.h`:
```cpp
private:
  RingbufHandle_t recvQueue = nullptr;
  StaticRingbuffer_t _recvQueueStruct;
  uint8_t _recvQueueStorage[RECV_QUEUE_SIZE * sizeof(mesh_message) + /* header overhead */ 128];
```
In `Mesh::begin()`:
```cpp
recvQueue = xRingbufferCreateStatic(sizeof(_recvQueueStorage), RINGBUF_TYPE_NOSPLIT, _recvQueueStorage, &_recvQueueStruct);
```
RX ISR callback:
```cpp
BaseType_t woken = pdFALSE;
xRingbufferSendFromISR(recvQueue, &msg, sizeof(msg), &woken);
if (woken) portYIELD_FROM_ISR();
```
Consumer in `loop()`:
```cpp
size_t item_size;
mesh_message* msg = (mesh_message*)xRingbufferReceive(recvQueue, &item_size, 0);
if (msg && item_size == sizeof(mesh_message)) {
  handleReceivedMessage(*msg);
  vRingbufferReturnItem(recvQueue, msg);
}
```
Remove old head/tail/count members + wrapping code.

- [ ] **Step 3: Enrollment::pendingRelayQueue → same treatment**

Similar structure. Item type is `{mac[6], pubKey[32]}` — 38 bytes.

- [ ] **Step 4: Build + host test + ESP-IDF + size**

Verify recv queue semantics preserved via existing e2e tests (multi-hop scenarios exercise recvQueue heavily).

- [ ] **Step 5: Commit + push + PR**

---

## Task 9 — Tickless PM + dedicated mesh task (EE)

**Files:**
- Modify: `firmware/sdkconfig.defaults` — add PM flags.
- Modify: `firmware/main/main.cpp` — `esp_pm_configure` + `xTaskCreatePinnedToCoreStatic` for mesh drain.
- Modify: `firmware/main/src/mesh/Mesh.cpp` — RX callback signals task via `xTaskNotifyFromISR`; loop body moves to task.

**Interfaces:**
- Consumes: Task 8's ringbuf (must be static, ISR-safe).
- Produces: no public API change.

**Hard prerequisites:** Tasks 3, 4, 5, 6, 7, 8 landed. Wake sources must be complete — any lingering polling loop defeats sleep.

- [ ] **Step 1: Baseline size + current-draw**

If multimeter available: probe battery leaf node current at rest. Record baseline.

- [ ] **Step 2: Add PM sdkconfig**

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_DFS_INIT_AUTO=y
CONFIG_FREERTOS_HZ=1000
```

- [ ] **Step 3: Configure PM in main**

```cpp
#include "esp_pm.h"
esp_pm_config_esp32_t pm_cfg = {
  .max_freq_mhz = 240,
  .min_freq_mhz = 80,
  .light_sleep_enable = true,
};
ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
```

- [ ] **Step 4: Create dedicated mesh task**

In main setup:
```cpp
static StackType_t mesh_task_stack[4096];
static StaticTask_t mesh_task_tcb;
static TaskHandle_t mesh_task_handle = nullptr;

extern "C" void mesh_task_fn(void*) {
  for (;;) {
    xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY);
    Mesh::getInstance().drain();  // process recvQueue
  }
}

mesh_task_handle = xTaskCreateStaticPinnedToCore(
  mesh_task_fn, "mesh", sizeof(mesh_task_stack) / sizeof(StackType_t),
  NULL, tskIDLE_PRIORITY + 3, mesh_task_stack, &mesh_task_tcb, 0);
Mesh::getInstance().setDrainNotifyHandle(mesh_task_handle);
```

- [ ] **Step 5: Wire RX-ISR → task notify**

In `Mesh.cpp` RX callback (after `xRingbufferSendFromISR`):
```cpp
BaseType_t woken2 = pdFALSE;
vTaskNotifyGiveFromISR(drainNotifyHandle_, &woken2);
if (woken || woken2) portYIELD_FROM_ISR();
```

Add `Mesh::drain()` public method: consumes recvQueue until empty, calls existing handleReceivedMessage for each.

Remove `Mesh::loop()`'s recvQueue drain — it's now in the dedicated task.

- [ ] **Step 6: Build + host test + ESP-IDF + size**

Host test: SimNode / world tick doesn't exercise real FreeRTOS tasks — tests should still pass since Mesh::drain is publicly callable and the sim harness can call it directly.

- [ ] **Step 7: Current measurement**

Flash a leaf. Probe with multimeter for 60 seconds at rest. Compare to Step-1 baseline. Acceptance: ≥ 25% reduction (target 30-40%).

If < 20% reduction, investigate: any polling loops in adapters? Any un-yielded busy waits? Any peripherals holding wake sources?

- [ ] **Step 8: Commit + push + PR**

---

## Task 10 — Drop CONFIG_AUTOSTART_ARDUINO, own app_main (AAA)

**Files:**
- Modify: `firmware/sdkconfig.defaults` — remove `CONFIG_AUTOSTART_ARDUINO=y`.
- Modify: `firmware/main/main.cpp` — delete `setup()` + `loop()`; write `app_main()`.
- Modify: `firmware/main/idf_component.yml` — remove `arduino-esp32` from REQUIRES if no header dep survives.
- Modify: files still `#include <Arduino.h>` — replace with narrower ESP-IDF includes.

**Interfaces:**
- Consumes: everything from Tasks 1-9 landed (all Arduino API surface must be gone).
- Produces: `app_main()` bootstraps entire firmware; FreeRTOS scheduler owns runtime.

**Prerequisite scrub — MUST pass before starting:**
```bash
grep -rn "digitalRead\|digitalWrite\|pinMode\|attachInterrupt\|Serial\.\|WiFi\.\|String \|delay(\|millis()" firmware/main/src/ | grep -v "// \|/\*"
```
Expected: zero matches (or only intentional-comment matches). Logger's internal Arduino Serial usage is a documented exception — but if kept, keep the Arduino.h include narrow (only Logger.cpp).

- [ ] **Step 1: Prerequisite scrub**

Run the grep above. Any match = a preceding task missed something. Fix in the task's PR (do NOT patch here) or open a small follow-up PR before starting Task 10.

- [ ] **Step 2: Baseline size**

- [ ] **Step 3: Write app_main scaffold**

In `main.cpp`:
```cpp
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_now.h>
#include <esp_pm.h>
#include <sodium.h>

extern "C" void app_main(void) {
  // 1. NVS
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  // 2. GPIO bundled config (from Task 7)
  configure_gpio_output_group();
  configure_gpio_input_group();
  ESP_ERROR_CHECK(gpio_install_isr_service(0));

  // 3. UART driver (Task 5)
  install_uart_driver();

  // 4. libsodium (Task 2)
  if (sodium_init() < 0) { /* fatal */ }

  // 5. Persistence layer
  lattice::eeprom::init();
  lattice::err_core::init();

  // 6. WiFi + ESP-NOW (Task 3)
  init_wifi_espnow();

  // 7. Adapter
  adapter = AdapterFactory::createFromEEPROM();
  adapter->init();

  // 8. Mesh
  Mesh::getInstance().begin(...);

  // 9. Dedicated mesh task (Task 9)
  spawn_mesh_task();

  // 10. Power management (Task 9)
  configure_pm();

  // app_main returns; FreeRTOS scheduler runs.
  // Adapter->loop() still needs a task — create small housekeeping task.
  spawn_housekeeping_task();
}
```

- [ ] **Step 4: Housekeeping task for adapter->loop + display + button**

```cpp
static StackType_t housekeeping_stack[4096];
static StaticTask_t housekeeping_tcb;
extern "C" void housekeeping_task_fn(void*) {
  for (;;) {
    adapter->loop();
    display_manager.tick();
    button_handler.tick();
    vTaskDelay(pdMS_TO_TICKS(10));  // 100 Hz housekeeping
  }
}
xTaskCreateStaticPinnedToCore(housekeeping_task_fn, "hk", ..., 0);
```

- [ ] **Step 5: Remove CONFIG_AUTOSTART_ARDUINO**

Delete from `sdkconfig.defaults`.

- [ ] **Step 6: Attempt to remove arduino-esp32 from idf_component.yml**

Delete the `espressif/arduino-esp32` entry. Try `idf.py build`. If it fails on missing headers (Arduino.h transitives), add narrower ESP-IDF includes to those files instead.

Sometimes arduino-esp32 is only pulled by transitive dependencies. If completely removable, expected additional ~40 KB flash.

- [ ] **Step 7: Build + host test + ESP-IDF + size**

Host test: many tests may use `Arduino.h`-defined types (String, etc). If Task 7 fully migrated, tests should pass on their own mocks now. If not, tests still link `Arduino.h` from `tests/mocks/`.

- [ ] **Step 8: Manual boot-to-idle firmware test**

Flash to a device. Serial log at boot should show Logger output. Verify:
- Node boots
- Attempts enrollment (if unenrolled)
- Or enters enrolled steady-state (if enrolled) — beacon RX, health tick, PM sleeping between events

If boot fails: rollback strategy is to re-add CONFIG_AUTOSTART_ARDUINO=y + restore setup()/loop() — but that means Task 10 needs another attempt with the specific issue understood.

- [ ] **Step 9: Commit + push + PR**

Commit message notes: Arduino API surface = zero (or lists remaining Logger exception), size delta, current-draw delta if measured.

---

## Post-plan checklist (for the orchestrator, not per-task)

After all 10 tasks land:
1. Full test suite pass at umbrella tip.
2. Cumulative size delta measured + logged in ledger.
3. Grep scrub confirms Arduino API absence.
4. Battery-node current-draw measurement recorded.
5. Broad final review dispatched per subagent-driven-development skill.
6. Umbrella PR opened to main with cumulative summary + all sub-PR references.
7. On merge to main: update memory with Phase I completion + total deltas.
