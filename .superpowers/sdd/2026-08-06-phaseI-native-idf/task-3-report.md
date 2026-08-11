# Task 3 Report: Raw WiFi + ESP-NOW init (items BB + ZZ)

## Status
**DONE**

## Branch / Commit
- Branch: `feat/phaseI-task3-raw-wifi` (base: `docs/phaseI-native-idf` @ `a6f8070`)
- Commit: `c9362fa` — feat(phaseI/task3): raw ESP-IDF WiFi + ESP-NOW init (items BB + ZZ)
- Pushed to `origin/feat/phaseI-task3-raw-wifi`. No PR opened (per instructions — orchestrator merges).

## Changes Implemented

### `firmware/main/src/mesh/Mesh.h`
- Removed `#include <WiFi.h>`.
- Added `#include <esp_netif.h>` + `#include <esp_event.h>` (`<esp_wifi.h>` and `<esp_now.h>` were already present).
- `<Arduino.h>` kept — still needed elsewhere in this file (`millis()`, `String` in Mesh.cpp; out of Task 3's scope, Task 7 handles that).

### `firmware/main/src/mesh/Mesh.cpp`
- `#include <WiFi.h>` → `#include <esp_netif.h>` + `#include <esp_event.h>`.
- `Mesh::setupWiFi()` body: replaced `if (!WiFi.mode(WIFI_STA)) { ... }` with the raw ESP-IDF bring-up sequence specified in the brief:
  ```cpp
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ```
  followed by the existing `esp_wifi_set_channel(...)` + `readMacAddress()` + `peers.setDeviceMac(...)` lines, unchanged.
- `esp_wifi_set_storage(WIFI_STORAGE_RAM)` makes Task 1's `CONFIG_ESP_WIFI_NVS_ENABLED=n` (once that PR lands) fully effective — WiFi driver state stays RAM-only, never touches flash.
- `Mesh::begin()`'s public `bool` return-value contract is preserved. No wire/frame changes — `esp_now_send`/`esp_now_register_recv_cb` call sites in `setupEspNow()` are untouched.

### Host test mocks (`tests/mocks/`)
Real `esp_wifi_init`/`esp_wifi_set_storage`/`esp_wifi_set_mode`/`esp_wifi_start`/`esp_netif_init`/`esp_event_loop_create_default` and the `wifi_init_config_t`/`WIFI_INIT_CONFIG_DEFAULT()`/`WIFI_STORAGE_RAM` symbols don't exist in the mock layer, so host tests wouldn't compile without adding them:
- `esp_wifi_mock.h` / `esp_wifi_mock.cpp`: added always-succeed mocks for the four `esp_wifi_*` calls above, plus `wifi_init_config_t` (empty placeholder struct — no test inspects its fields) and `WIFI_INIT_CONFIG_DEFAULT()`. Added `mockWifiStarted` flag (set true by the mock `esp_wifi_start()`) for any future test that wants to assert bring-up happened, and reset it in `resetWifiMock()`.
- New `tests/mocks/esp_netif.h` and `tests/mocks/esp_event.h` — small header-only mocks, each with one inline always-`ESP_OK` function. Header-only, so no `CMakeLists.txt` source-list change needed.
- `esp_err.h`: added an `ESP_ERROR_CHECK(x)` macro (guarded `#ifndef`) that evaluates once and discards the result — the real macro aborts the process on failure, but nothing in the host suite exercises that path since every mock always returns `ESP_OK`.
- `WIFI_STA`/`WIFI_MODE_STA` were previously defined unconditionally in `WiFi.h` only. Since `Mesh.h`/`Mesh.cpp` no longer include `WiFi.h` directly (it's still pulled in transitively by `esp_now_mock.h`, which needs `wifi_tx_info_t`/`esp_now_send_status_t` from it), I guarded both `WiFi.h`'s and the new `esp_wifi_mock.h`'s definitions with `#ifndef WIFI_STA` so whichever header a given translation unit includes first "wins" without a macro-redefinition warning, regardless of include order across the ~4 files that reach `esp_wifi.h`/`WiFi.h` by different paths (`Mesh.h`, `hw_mac.h`, `Adapter.cpp`, `main.cpp`).

### `firmware/main.cpp`
No changes. WiFi bring-up stays entirely inside `Mesh::setupWiFi()` — confirmed via grep that `main.cpp` has zero direct `WiFi.` references, so there was nothing to move up a layer. (Per the task brief's parallelization note: this means no `// Phase I Task 3: raw WiFi` marker was needed in `main.cpp`, since main.cpp is untouched.)

## LWIP TCP/UDP status

**Not applicable on this branch — documented per the delivery instructions either way.**

This branch was built directly off `docs/phaseI-native-idf` @ `a6f8070`, which predates Task 1. `feat/phaseI-task1-sdkconfig-trim` exists as a branch name but has no commits beyond `a6f8070` in this checkout (`git rev-parse` shows it identical to `a6f8070`), so Task 1's sdkconfig changes — including the `CONFIG_LWIP_TCP_ENABLED=n` / `CONFIG_LWIP_UDP_ENABLED=n` flags this step is supposed to try re-enabling — are not present in `firmware/sdkconfig.defaults` on this branch at all (confirmed via `grep -n LWIP firmware/sdkconfig.defaults` — zero matches).

There is nothing to flip here. **Action item for whoever merges Task 1 and Task 3 into the umbrella branch:** once both land together, add (or re-enable if Task 1 had them commented out)
```
CONFIG_LWIP_TCP_ENABLED=n
CONFIG_LWIP_UDP_ENABLED=n
```
and rebuild — now that `WiFi.mode()` (which arduino-esp32 implements via lwIP-touching netif setup) is gone in favor of the raw `esp_wifi_*` sequence, these flags are much more likely to build cleanly. Not verified in this task since the flags don't exist yet on this branch's base.

## Size Measurement

`idf.py size`, clean builds (`rm -rf build`, full `reconfigure` + `build`) both before and after, `esp32` target, IDF v5.5.1:

| Section | Before (a6f8070) | After (c9362fa) | Delta |
|---|---:|---:|---:|
| `.text` (Flash Code) | 638,004 | 561,508 | **-76,496 B (-12.0%)** |
| `.rodata` (Flash Data) | 134,900 | 118,452 | **-16,448 B (-12.2%)** |
| IRAM `.text` | 92,727 | 92,635 | -92 B |
| `.bss` (DRAM) | 21,792 | 21,176 | -616 B |
| `.data` (DRAM) | 15,984 | 15,936 | -48 B |
| **Total image** | 882,931 | 789,847 | **-93,084 B (-10.5%)** |

This is a much larger win than the plan's Task 3 section anticipated (it only mentions BB/ZZ as an API-surface swap). The `.text`/`.rodata` drop is disproportionate to "replace one function call with six" — root cause: **`Mesh.cpp` was the only file in the entire firmware that referenced the global `WiFi` object** (confirmed via `grep -rn "WiFi\." firmware/main/main.cpp` → zero hits, and no other file includes `<WiFi.h>`). Once that single reference is gone, the linker no longer needs to pull in arduino-esp32's `WiFiGeneric`/`WiFiSTA`/`WiFiAP` translation units (and whatever those transitively reference) from the static `libarduino-esp32.a` archive — those are C++ classes with their own vtables/statics that were previously dead weight riding along just because `WiFi.mode()` was called once.

Verified this wasn't a build-inconsistency artifact: measured with a fully clean (`rm -rf build`, fresh `reconfigure`) build on both sides, using `git stash push -u` to isolate exactly the Task 3 source diff with no other confound (same `sdkconfig.defaults`, same `managed_components`, same submodule commit).

Bootloader/partition unaffected. `Smallest app partition is 0x100000 bytes` — free space at the app partition level rose from `0x286a0` (16%) pre-change to `0x3f230` (25%) post-change per the `idf.py build` size-check line.

## Tests

**Host: 293/293 pass** (unchanged count — this task added no new tests, consistent with the brief since it's an internal init-sequence swap with no new observable behavior).
```
cd tests && rm -f ../firmware/main/config/master_pubkey_pin.h
cmake --build build -j2 && ctest --test-dir build --parallel 1
```
Result: `100% tests passed out of 293`.

**ESP-IDF: clean build succeeds.** `esp32` target, IDF v5.5.1, `-j2`-equivalent cap (`CMAKE_BUILD_PARALLEL_LEVEL=2`, since `idf.py build` doesn't take a `-j` flag directly — confirmed via `idf.py build --help`). No new warnings introduced by this change; the one pre-existing warning seen in both before/after builds (`Mesh.cpp: ignoring attribute 'section(".iram1.2")' because it conflicts with previous 'section(".iram1.0")'` on the `dataRecvTrampoline`/`onDataRecvCallback` pair) was independently reproduced on the unmodified baseline build too, so it's not something Task 3 introduced.

## Verification Checklist
- [x] No wire changes — only `Mesh::setupWiFi()`'s internals changed; `setupEspNow()`, `esp_now_send`/`esp_now_register_recv_cb` call sites, and all message-building code untouched.
- [x] `Mesh::begin()` public semantic preserved (still returns `bool`, still leaves the radio in STA mode with ESP-NOW ready on success).
- [x] `master_pubkey_pin.h` deleted before every host test run.
- [x] clang-format 18 (`/opt/homebrew/opt/llvm@18/bin/clang-format --style=file`) run on `Mesh.h`/`Mesh.cpp` and all touched mock files — no diff from a second pass (idempotent).
- [x] 293/293 host tests pass.
- [x] ESP-IDF build succeeds (clean, from-scratch).
- [x] `main.cpp` untouched — no marker comment needed since there was nothing to add there.
- [x] Commit pushed to `origin/feat/phaseI-task3-raw-wifi`. No PR opened.

## Concerns / Notes for Orchestrator

1. **Failure-mode change in `setupWiFi()`.** The old code called `lattice::err::fail(...)` and returned `false` on `WiFi.mode()` failure — the caller (`Mesh::init()`) would then also return `false`, letting the firmware continue in a degraded (radio-less) state via whatever recovery/escalation path `err::fail` triggers. The brief's exact code snippet uses `ESP_ERROR_CHECK(...)` for the six new calls, which **aborts the process (`abort()`) on non-`ESP_OK`** instead. I followed the brief verbatim since it's explicit ESP-IDF idiom for boot-critical, essentially-never-fails calls (same pattern the plan itself uses for `nvs_flash_init()` in Task 4/10). Net effect: `setupWiFi()` now only returns `false` in theory (never in practice, since ESP_ERROR_CHECK would already have aborted); `Mesh::init()`'s `if (!setupWiFi()) return false;` guard is effectively dead code for this call now. Flagging this in case the orchestrator wants a softer failure path here instead of a hard abort — I judged it acceptable since a WiFi/ESP-NOW subsystem that fails to initialize at boot leaves the node unable to do its job anyway, and this matches the plan's own idiom elsewhere.
2. **LWIP flags don't exist yet on this branch** — see LWIP section above. This is a merge-order artifact (Task 3 was dispatched off the umbrella tip before Task 1 landed), not a Task 3 defect. Whoever merges needs to re-run the LWIP TCP/UDP=n attempt after both Task 1 and Task 3 are combined.
3. **Mesh.h/Mesh.cpp is shared with Task 8** (per the parallelization note) — Task 8 replaces `recvQueue[]`/head/tail with `RingbufHandle_t`, touching different regions of the same two files (the `RecvQueueEntry`/`recvQueueHead`/`recvQueueTail` members and `drainRecvQueue()`, well away from `setupWiFi()`/the include block this task touched). Should merge cleanly but worth a diff-review at merge time since both tasks touch the same two files.
4. **Untracked build artifacts** (`firmware/dependencies.lock`, `firmware/managed_components/`, `firmware/sdkconfig`) were generated by `idf.py reconfigure`/`build` in this worktree and are **not gitignored at the repo root** (only `build/` is). I deliberately did not `git add` them — consistent with the plan's own Task 1 baseline steps (`rm -rf build sdkconfig managed_components` before every reconfigure), which treat these as regenerable local artifacts. Flagging as a pre-existing gap in `.gitignore` (not something I fixed, out of scope) in case the orchestrator wants a follow-up to add `managed_components/`, `sdkconfig`, and `dependencies.lock` to the root `.gitignore`.
