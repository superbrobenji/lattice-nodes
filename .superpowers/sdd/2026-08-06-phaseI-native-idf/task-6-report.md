# Task 6 Report: esp_timer + PeerRegistry per-key NVS + stack shrink + nano-format (items FF+JJ+II+NN)

## Status
**DONE**

## Branch / Commit
- Branch: `feat/phaseI-task6-esp-timer` (base: `origin/docs/phaseI-native-idf` @ `31ca5ed`, umbrella tip with Tasks 1+2+3+4+5+8 merged + Task 2 follow-up)
- Commit: `4a0bbbc` — `feat(phaseI/task6): esp_timer + PeerRegistry per-key NVS + stack shrink + nano-format (items FF+JJ+II+NN)`
- Pushed to `origin/feat/phaseI-task6-esp-timer`. No PR opened (per instructions).

## Test results
- **Host: 296/296 pass** (`rm -f firmware/main/config/master_pubkey_pin.h && cd tests && cmake -B build . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ctest --test-dir build --parallel 1`).
- **ESP-IDF build: clean.** `idf.py reconfigure && idf.py build -- -j2` (env: `IDF_PATH=$HOME/esp/esp-idf`, IDF v5.5.1) succeeds with no errors and no new warnings — the only warnings in the full build log (`unknown kconfig symbol ...LTO/LWIP_*`, arduino-esp32's PSK `#warning`, `ErrorCore.cpp` noreturn, `main.cpp` `uart_config_t` missing-initializer, `Mesh.cpp` IRAM section-attribute conflict) are all pre-existing, from Tasks 1/5/8, unrelated to this task's edits.
- Firmware submodule `firmware/main/lib/lattice-protocol` was uninitialized in this fresh worktree (same issue flagged by Tasks 4/5/8); ran `git submodule update --init --recursive` before the first build.

## Per-sub-item status

### FF — millis() -> esp_timer_get_time()/1000ULL: **DONE**
- **Site count reality check (Step 1):** the brief estimated "~100" `millis()` sites; the actual codebase had **37** (`grep -rn "millis()" firmware/main/ --include="*.cpp" --include="*.h"`), across 11 files. Swept all of them — `grep -rn "millis()" firmware/main/` now matches only comments documenting the swap.
- Every site: `millis()` → `static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL`, inlined at each call site (no shared helper function) to keep the diff literal and easy to audit against the brief's sweep pattern.
- **Field type migrations** (uint32_t → uint64_t, cascading through every comparison/assignment that touches them):
  - `PeerRegistry.h`: `PeerInfo::lastSeenMillis` → **renamed** `lastSeenMs` (uint64_t) — matches `ReplayCache::Entry::lastSeenMs`'s existing naming, and the rename was safe to do repo-wide via `sed` since `NeighborTable`/`RouteTable`'s own `lastSeenMillis` fields are private nested-`Entry` members never referenced outside those two headers (verified by grep before renaming).
  - `NeighborTable.h`: `Entry::lastSeenMillis` → `lastSeenMs` (uint64_t); `nowMillis` params → `nowMs` (uint64_t) across `observe`/`observeAndMinDistance`/`selectNextHop`/`minFreshDistance`/`allocateSlot`.
  - `RouteTable.h`: `Entry::lastSeenMillis` → `lastSeenMs` (uint64_t); `record()`'s `nowMillis` → `nowMs` (uint64_t).
  - `ReplayCache.h`: `Entry::lastSeenMs` uint32_t → uint64_t; `isReplay()`'s `nowMs` uint32_t → uint64_t.
  - `Mesh.h`: `lastMasterBeaconReceivedMs`, `relayPendingAt`, `lastBeaconMs`, `lastRouteReportMs` → uint64_t. `lastBeaconMillis` retyped to uint64_t but **not** renamed to `lastBeaconMs` — that name was already taken by a separate, pre-existing dead field (zero-initialized in the constructor, never read/written elsewhere; out of this task's scope to remove). `testMillisNow()` (UNIT_TEST-only accessor) return type uint32_t → uint64_t.
  - `Adapter.h`/`.cpp`: `_lastHealthMillis` → uint64_t; `healthTickDue`/`resetHealthTick`'s `now` param → uint64_t.
  - `PirAdapter.h`/`.cpp`: `_lastTrigger` → uint64_t (not in the brief's header list, but required — cascades from `Adapter::healthTickDue`'s new signature and `PirAdapter::loop()`'s own `now` local).
  - `Button.h`/`.cpp`: `_lastPollMs` → uint64_t. `waitForHold(uint32_t ms)`'s `ms` parameter stays uint32_t (it's a *duration*, not a timestamp — no wraparound concern).
  - `DisplayManager.h`, `ButtonHandler.h`, `SevenSegDisplay.cpp`: local `static`/stack timestamp variables (`lastToggleMs`, `holdStart`, `confirmDeadline`, `start`) → uint64_t. `ButtonHandler::HOLD_MS` → uint64_t.
  - `SerialAdapter.cpp`, `main.cpp`: local `now`/`lastEnrollmentBroadcast` → uint64_t.
  - `mac_table.h`: `evict_oldest_by_ts()` widened from a hardcoded `uint32_t` `memcpy`/compare to `uint64_t` — its only two callers (`RouteTable`, `ReplayCache`) both migrated their timestamp field in this task, so this was a clean cut, not a compatibility shim (grep-verified no other caller exists).
- **Host mock:** added `tests/mocks/esp_timer.h` — `extern "C" inline int64_t esp_timer_get_time(void)` backed by the *same* `_mockMillis` clock `tests/mocks/time_mock.h`'s `millis()`/`advanceMillis()`/`resetMillis()` already drive, so every existing test that calls `advanceMillis()` continues to work unmodified against the migrated `esp_timer_get_time()` call sites — no test rewrites needed beyond the field rename.
- **NN prerequisite sweep (Step 3):** `grep -rn "%u\|%lu\|%d\|%ld" firmware/main/src/` filtered for ms-related keywords found exactly one specifier referencing a migrated field: `SerialAdapter.cpp`'s `SIMULATE_MODE`-gated peer-dump line (`p.lastSeenMs`, `%lu` → `%llu`, cast `(unsigned long)` → `(unsigned long long)`). See NN below for why this one site also needed a second look after the nano-format flag was added.

### JJ — PeerRegistry per-key NVS blob: **DONE**
- Real `nvs_get_blob` (verified against local `$HOME/esp/esp-idf/components/nvs_flash/include/nvs.h` v5.5.1, matching the plan's corrected Step 5 note) has no offset/partial-read mode — whole-blob only. Per-key naming (`peer0`..`peer9`) is the only path to eliminate the 380-byte (`MAX_PEERS(10) * PEER_RECORD_SIZE(38)`) stack buffer.
- Added three new `lattice::eeprom::*` functions (`EepromManager.h`/`.cpp`): `loadPeerRecord(uint8_t index, uint8_t* record)`, `savePeerRecord(uint8_t index, const uint8_t* record)`, `erasePeerRecord(uint8_t index)` — each opens/commits/closes its own short-lived `nvs_handle_t` against key `"peer" + index` (built by a new `peerKey()` helper), following the exact same per-operation-handle idiom Task 4 established for every other `EepromManager` accessor.
- `PeerRegistry::loadFromEEPROM()`/`saveToEEPROM()` rewritten to loop `0..MAX_PEERS` calling the per-record primitives directly, using a single reused 38-byte `uint8_t record[...]` buffer instead of the old 380-byte combined array. `saveToEEPROM()` explicitly **erases** slots `[peerCount, MAX_PEERS)` on every save — unlike the old single-blob design (where a shorter blob simply overwrote the longer one), each `peerN` key now persists independently, so a shrink (peer removal) must explicitly clear the vacated key or it would leak stale data back in on the next load.
- `EepromManager`'s existing batch API (`loadPeerList`/`savePeerList`/`hasPeers`/`clearPeerList`) was **kept, not removed** — its callers (`main.cpp`'s default-peer bootstrap on first boot, and 8 existing `tests/unit/test_eeprom_manager.cpp` cases exercising the batch shape) don't need per-record streaming, so these four functions were reimplemented as thin loops over the three new primitives instead of changing their signatures. This preserves both external call sites' contracts unchanged while still moving the actual on-flash representation to per-key storage underneath.
- `NVS_KEYS::PEER_LIST` ("peers") constant left in place but marked dead/retired in a comment (migration/rollback reference) — nothing reads or writes that key anymore.
- **On-flash layout change confirmed and accepted:** single `"peers"` blob → `"peer0".."peer9"` keys. **Requires device reflash on Phase I close** (per project posture — reflash-on-major-release). Flagging for the Phase I release notes as instructed.

### II — stack high-water diagnostic + shrink: **DONE (compile-time estimate, not live measurement)**
See "Stack sizing rationale" below for the full analysis. `CONFIG_ARDUINO_LOOP_STACK_SIZE` flipped `8192` → `4096` in `firmware/sdkconfig.defaults`. No temporary diagnostic block was added to `main.cpp`'s `loop()` — this worktree cannot flash real hardware, so a live `uxTaskGetStackHighWaterMark()` reading (plan Step 6) was not obtainable; substituted a `-fstack-usage`-based static analysis instead, per the delegating instructions ("use compile-time estimate + reserve conservatively. Document any estimate assumption").

### NN — nano-format: **DONE**, with a corrected assumption
`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` added. **Finding:** ESP-IDF's own Kconfig help text (`$HOME/esp/esp-idf/components/newlib/Kconfig`, `LIBC_NEWLIB_NANO_FORMAT` option) states nano-format "doesn't support 64-bit integer formats" — full stop, not "supports them if you use the right specifier." This contradicts the Task 6 plan's Step 8 assumption ("Any %llu/%lld failing = missed a specifier in Step 3"): under nano-format, a *correct* `%llu` specifier still won't render a `long long` argument correctly, because the nano `vfprintf` implementation doesn't have a 64-bit code path at all.

Given that, flipping nano-format is only safe if the firmware never actually exercises a `%llu`/`%lld` specifier at runtime. Checked: `grep -rn "%ll" firmware/main/` after the FF sweep turns up exactly one site — `SerialAdapter.cpp`'s `SIMULATE_MODE`-gated peer-dump `Serial.printf`, printing the now-`uint64_t` `PeerInfo::lastSeenMs`. `SIMULATE_MODE` defaults to `0` (`firmware/main/project_config.h`), so that whole `#if SIMULATE_MODE` block — including the one `%llu` — is **not compiled into a default/production build**. Nano-format is therefore safe to enable for the shipping configuration. Documented this precisely in both the sdkconfig comment and the commit message so a future `SIMULATE_MODE=1` debug build doesn't quietly ship a garbled (though harmless — cosmetic debug output only, no wire/safety impact) peer-dump line.

## Stack sizing rationale (II)

No flashable hardware was available from this worktree, so instead of a live `uxTaskGetStackHighWaterMark()` reading, I built the firmware with `-fstack-usage` (`idf.py -D CMAKE_C_FLAGS="-fstack-usage" -D CMAKE_CXX_FLAGS="-fstack-usage" reconfigure && idf.py build -- -j2`), which makes the xtensa GCC toolchain emit a `.su` file per translation unit recording each function's own (compiler-verified, not guessed) worst-case stack frame size. This gives real per-function numbers instead of hand estimates.

Traced the deepest realistic call chains rooted at `loop()` (the Arduino loop task — the only task whose stack this sdkconfig knob controls) and summed the **own-frame** sizes along each chain (each `.su` entry reports only that function's own frame, not its callees', so the call chain's peak is the sum of frames simultaneously live on the stack):

**Deepest chain found — master node forwarding a received route report to serial:**
```
main::loop()                                     48
  Mesh::loop()                                    32
    Mesh::drainRecvQueue()                       272
      Mesh::processRouteReport(msg)              320   (master branch)
        externalRecvCallback -> dataRecvCallback   48
          Adapter::onMeshData                      32
            SerialAdapter::onMeshDataImpl         304
              SerialFraming::encode                352
                                                 -----
                                                 1408 bytes
```
**Second-deepest — non-master relaying uplink ADAPTER_DATA (transmitCore's own frame is the single largest in the codebase):**
```
main::loop() 48 + Mesh::loop() 32 + drainRecvQueue() 272 + processAdapterData() 256
  + Mesh::transmitCore() 496 + deepest sequential callee (crypto::sealPayload, 112)
                                                 -----
                                                 1216 bytes
```
**Enrollment/JOIN_ACK persistence path (exercises the JJ-touched code):**
```
main::loop() 48 + Mesh::loop() 32 + drainRecvQueue() 272 + Mesh::processJoinAck() 240
  + Enrollment::processJoinAck() 32 + registerPeerWithKeyTrampoline() 32
  + Mesh::registerPeerWithKey() 80 + PeerRegistry::saveToEEPROM() 80
  + EepromManager::savePeerRecord()/erasePeerRecord() 48
                                                 -----
                                                  864 bytes
```

The deepest verified chain (1408 B) leaves **2688 B of headroom inside the new 4096 B budget**, or — reframed to match the plan's original acceptance criterion ("if high-water > 4096 B free [at the current 8192 B stack], safe to flip to 4096") — 8192 − 1408 = 6784 B of free stack at the old size, comfortably over the 4096 B bar.

**What this analysis does *not* cover:** `-fstack-usage` was only applied to `main`-component sources. Vendored library internals — libsodium's `crypto_aead_chacha20poly1305_ietf_*`/`crypto_auth_hmacsha256` (called from `E2ECrypto.h`/`RouteMac.h`, both inside the `transmitCore`/`processRouteReport` chains above), ESP-NOW, `nvs_flash`, and the UART driver — are not instrumented, so their contribution to the true peak isn't in the 1408 B figure. This is a real gap, but it is **not a Task 6 regression**: Task 6 didn't touch any of these call sites' structure (FF only swapped the clock source; JJ only touched `PeerRegistry`'s EEPROM load/save, which isn't in the `transmitCore`/`processRouteReport` hot path at all) — whatever unmeasured library depth exists today is identical to what Tasks 1–5 already shipped and validated against the *old* 8192 B stack. Task 6's own net effect on stack usage is mildly **negative** (JJ: `PeerRegistry::loadFromEEPROM`/`saveToEEPROM` frames measured at 128 B / 80 B — down from an implied ~400+ B with the old 380-byte array — an unambiguous reduction) to roughly neutral (FF's uint32→uint64 widening adds a handful of bytes per frame holding a timestamp local, already included in the 1408 B figure above).

**Recommendation:** proceed with `CONFIG_ARDUINO_LOOP_STACK_SIZE=4096` as the plan specifies, but confirm with a real `uxTaskGetStackHighWaterMark()` reading (Step 6 as originally written) at the next hardware-in-loop opportunity, before this configuration reaches a production fleet — the sdkconfig comment left at this setting says the same. This is standard confirmatory practice, not a blocker to landing Task 6.

## Size Delta (`idf.py size`)

Baseline is Task 8's own reported post-Task-8 numbers (`.superpowers/sdd/2026-08-06-phaseI-native-idf/task-8-report.md`) — Task 8's commit `31ca5ed` (merged into the umbrella) is this branch's exact base commit, so no separate baseline rebuild was needed.

| Section | Baseline (Task 8 / `31ca5ed`) | Post-Task-6 (FF+JJ, before II/NN flags) | Final (FF+JJ+II+NN) | Delta (final vs. baseline) |
|---|---:|---:|---:|---:|
| .text (Flash Code) | 579,108 | 580,000 | 548,036 | **-31,072** |
| .rodata (Flash Data) | 118,288 | 118,304 | 115,960 | **-2,328** |
| IRAM (.text + .vectors) | 92,803 | 92,807 | 92,787 | **-16** |
| .bss | 21,496 | 21,800 | 21,800 | **+304** |
| .data | 16,196 | 16,196 | 16,196 | 0 |
| **Total image** | **806,683** | 807,595 | **773,267** | **-33,416** |

Read left-to-right: FF+JJ's code changes alone are a wash-to-slightly-negative (+892 B `.text` from 64-bit arithmetic being inherently pricier than 32-bit on this target — extra instructions for widened compares/subtracts and the `/1000ULL` divisions — largely offset by JJ's per-record loop replacing the old batch-copy code; +304 B `.bss` from every migrated `uint32_t` timestamp field costing 4 more bytes × however many array entries it lives in, e.g. `NeighborTable`/`RouteTable`/`ReplayCache`/`PeerRegistry`'s fixed-size entry arrays, all statically embedded in the global `Mesh` instance). **NN (nano-format) is the dominant win** — flipping it alone accounts for essentially the entire final `.text`/`.rodata` reduction (~34 KB combined), matching the plan's expectation ("`.text` down ~20-30 KB") and landing at the high end of that estimate.

**Correction to the plan's `.bss` expectation:** the plan expected "`.bss` down 4 KB (stack shrink)" from II. That does not show up in the static size table above (`.bss` is unchanged between the pre-flip and final columns) — checked why: `arduino-esp32`'s `loopTask` is created via `xTaskCreateUniversal()` (`managed_components/espressif__arduino-esp32/cores/esp32/main.cpp:113`), which allocates its stack from the **heap** at boot (`pvPortMalloc`-backed), not from a static array. `CONFIG_ARDUINO_LOOP_STACK_SIZE` therefore controls a *runtime heap allocation size*, invisible to `idf.py size`'s link-time section accounting — the real effect of the 8192→4096 shrink is **~4 KB more free heap at boot**, not a `.bss` reduction. Flagging this as a plan-assumption correction, in the same spirit as Task 4's `nvs_get_blob` offset-mode correction.

## Concerns / Follow-ups
- **JJ requires device reflash on Phase I close** — on-flash NVS layout for the peer list changed (`"peers"` → `"peer0".."peer9"`). Needs to land in the Phase I release notes as an explicit reflash requirement (accepted per project posture; not a regression, a deliberate trade for removing the 380 B stack buffer).
- **II is a compile-time estimate, not a live measurement** — see rationale above. Low risk (2.7 KB headroom against the verified chain, and Task 6's own delta on stack usage is net-negative-to-neutral), but a real `uxTaskGetStackHighWaterMark()` reading is still recommended before production shipment, and the sdkconfig comment says so.
- **NN's one `%llu` call site is SIMULATE_MODE-gated** — safe for production (default `SIMULATE_MODE=0`), but if a future task ever ships a `SIMULATE_MODE=1` debug build with nano-format still enabled, that one peer-dump log line will render `lastSeenMs` incorrectly (cosmetic only). Left a comment at both the sdkconfig flag and the call site.
- **Site-count correction:** the Task 6 brief/plan estimated "~100" `millis()` sites and "~30-40 files"; the actual codebase had 37 sites across 11 files. Not a problem, just flagging the estimate-vs-reality gap for whoever maintains the umbrella plan's running notes.
- `firmware/dependencies.lock`, `firmware/managed_components/`, `firmware/sdkconfig` were generated by the ESP-IDF build in this worktree and were **not** committed (same exclusion Task 8 flagged — not covered by `.gitignore`, excluded via explicit `git add <paths>` rather than `git add -A`).
