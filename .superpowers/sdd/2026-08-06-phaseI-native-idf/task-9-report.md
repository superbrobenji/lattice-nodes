# Task 9 Report — Tickless PM + dedicated mesh task (item EE)

**Status:** Complete, with the current-measurement gate (plan Step 7) explicitly **skipped** — no flashable hardware in this session. Everything else (sdkconfig, `esp_pm_configure`, dedicated mesh task, host tests, ESP-IDF build, size delta) is done and verified.

**Branch:** `feat/phaseI-task9-tickless-pm` (off `origin/docs/phaseI-native-idf` @ `2499bb7`), pushed to origin. No PR opened (per instructions — orchestrator handles).

**Commit:** `99033f0` — "feat(phaseI/task9): tickless PM + dedicated mesh task (item EE)"

## Test results

- **Host tests: 296/296 pass** (`cd tests && cmake --build build -j2 && ctest --test-dir build --parallel 1`), pin file removed first (`rm -f firmware/main/config/master_pubkey_pin.h`).
- **ESP-IDF build: clean.** `idf.py reconfigure && idf.py build -- -j2` (env: `IDF_PATH=$HOME/esp/esp-idf`, IDF v5.5.1) succeeds with 0 errors and exactly 1 warning, which is pre-existing and unrelated to this task (`Mesh.cpp:356` `IRAM_ATTR` section-attribute conflict on `onDataRecvCallback`, present since Task 7/8). `idf.py size` runs clean.
- Two environment fixes needed before either build worked, neither specific to this task:
  - `firmware/main/lib/lattice-protocol` submodule was uninitialized in this fresh worktree (`git submodule update --init --recursive`) — same issue flagged in Task 5/8 reports for parallel worktrees.
  - Neither `ninja` nor `clang-format-18` were on `PATH`. Used `cmake -G "Unix Makefiles"` for the ESP-IDF build (GNU Make 3.81 was available) and `pip3 install --target <scratch> clang-format==18.1.5` (PyPI ships prebuilt clang-format binaries) for the formatting pass, since Homebrew only had `clang-format@22` and `clang-format@11`.

## PM sdkconfig

Added to `firmware/sdkconfig.defaults`:
```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_DFS_INIT_AUTO=y
```
`CONFIG_FREERTOS_HZ=1000` was already set (Task 6) — verified, not re-added.

## Size delta (`idf.py size`, isolated via `git stash` of the tracked source changes to get a true before/after on this branch, same methodology as Task 8's report)

| Section | Baseline (2499bb7) | Post-Task-9 | Delta |
|---|---|---|---|
| .text (Flash Code) | 546,356 | 548,560 | +2,204 |
| .rodata + .appdesc (Flash Data) | 115,808 | 117,792 | +1,984 |
| IRAM (.text + .vectors) | 92,831 | 101,075 | +8,244 |
| .bss | 21,360 | 25,976 | +4,616 |
| .data | 16,196 | 16,260 | +64 |
| **Total image** | 771,223 | 783,719 | **+12,496** |

This is a real, expected cost, not a regression to chase down:
- **IRAM +8,244 bytes** is the single largest line item. ESP-IDF's PM/DFS and tickless-idle implementations (frequency-switch sequencing, the `portSUPPRESS_TICKS_AND_SLEEP` tickless port hook) run with the flash cache disabled during the actual frequency/sleep transition, so that code must be IRAM-resident — this is inherent to turning `CONFIG_PM_ENABLE`/`CONFIG_FREERTOS_USE_TICKLESS_IDLE` on, not something this task's application code controls.
- **.bss +4,616 bytes**: `mesh_task_stack[4096]` (the dedicated task's static stack — `StackType_t` is 1 byte on this Xtensa port, confirmed by the delta closely matching 4096 + the small addition of `mesh_task_tcb`/`drainNotifyHandle_`) plus `esp_pm`'s own internal lock/DFS bookkeeping state now linked in.
- **.text/.rodata** (+2,204 / +1,984): the `esp_pm` component's code and Kconfig-driven constant tables (lock names, frequency tables) being linked in for the first time.

Flash headroom remains comfortable: `lattice-nodes.bin` is `0xbf5e0` (783,712) bytes against a `0x100000` (1,048,576) byte app partition — 25% free, down from 26% pre-Task-9.

## What changed

- `firmware/sdkconfig.defaults` — added the three PM flags above.
- `firmware/main/main.cpp`:
  - `esp_pm_configure(&pm_cfg)` (max 240MHz / min 80MHz / light sleep enabled) added immediately after Task 4's `nvs_flash_init()` block in `setup()`, using `esp_pm_config_t` (not the brief's `esp_pm_config_esp32_t` — that typedef is deprecated in this IDF 5.5.1 checkout in favor of the chip-generic `esp_pm_config_t`; using the deprecated alias would have compiled but emitted an unnecessary deprecation warning).
  - Dedicated mesh-drain task: `static StackType_t mesh_task_stack[4096]` + `static StaticTask_t mesh_task_tcb` + `static TaskHandle_t mesh_task_handle` (all static — no heap), `mesh_task_fn` blocks on `xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY)` and calls `Mesh::getInstance()->drain()` on each notify. Created via `xTaskCreateStaticPinnedToCore(..., tskIDLE_PRIORITY + 3, mesh_task_stack, &mesh_task_tcb, 0)` right after `mesh.init()` succeeds in `setup()`, immediately followed by `mesh.setDrainNotifyHandle(mesh_task_handle)`.
- `firmware/main/src/mesh/Mesh.h`:
  - Added `#include <freertos/task.h>`.
  - New private member `TaskHandle_t drainNotifyHandle_ = nullptr`.
  - New public `void drain()` — thin wrapper calling the existing (still-private) `drainRecvQueue()`. Kept `drainRecvQueue()` itself unchanged/private rather than renaming it, so the existing unit tests that call `mesh.drainRecvQueue()` directly (exposed via the `UNIT_TEST`-makes-everything-public convention — see `test_mesh_logic.cpp`, `test_route_report.cpp`) keep working unmodified.
  - New public `void setDrainNotifyHandle(TaskHandle_t handle)`.
- `firmware/main/src/mesh/Mesh.cpp`:
  - `onDataRecvCallback` (ISR context): after the existing `xRingbufferSendFromISR`, added a null-checked `vTaskNotifyGiveFromISR(instance->drainNotifyHandle_, &woken2)`, with `portYIELD_FROM_ISR()` now firing if either the ring-buffer send or the notify woke a higher-priority task.
  - `Mesh::loop()`: removed the `drainRecvQueue();` call that used to be its first statement. Everything else in `loop()` (EEPROM flush, enrollment-relay drain, route-report timer, deferred beacon relay, master beacon) is unchanged.
- `tests/e2e/harness/SimNode.cpp`: `SimNode::tick()` now calls `mesh_->drain()` explicitly (immediately before `mesh_->loop()`, matching the old internal ordering), since the harness has no real FreeRTOS task to be woken by a notify and `Mesh::loop()` no longer drains on its own.
- `tests/mocks/freertos/task.h` (new): shadow header for `<freertos/task.h>`, providing `TaskHandle_t` (`void*`) and a no-op `vTaskNotifyGiveFromISR()` mock so `Mesh.h`/`Mesh.cpp` compile on the host build. `main.cpp` (which creates the real task and calls `setDrainNotifyHandle()`) is never compiled into host tests, so `drainNotifyHandle_` is always `nullptr` in every host/SimNode test — the ISR mock is present only to satisfy the compiler, never meaningfully exercised.

No wire format changes. No new heap allocation. Public API surface is additive only (`Mesh::drain()`, `Mesh::setDrainNotifyHandle()` are new public methods; nothing existing was removed from the public interface).

## Wake-source enumeration + analysis (in place of Step 7's hardware current measurement)

Per-file grep for `delay(`/`while(true)`/`for(;;)` across `firmware/main/src` to find anything that could keep the CPU busy or artificially short-circuit tickless idle's ability to extend sleep:

**Confirmed non-blocking / event-driven (no busy loops in the steady-state hot path):**
- `main.cpp`'s `loop()` body: `lattice::err_core::drainPendingBlink()`, `greenLed.update()`/`redLed.update()`, `lattice::err_core::tick()`, `mesh.loop()`, `mesh.checkMasterTimeout()`, `DisplayManager::tick()`, `adapter->loop()`, `ButtonHandler::tick()` are all single-pass, non-blocking checks — none call `delay()` or spin.
- `Mesh::loop()` (post-Task-9): EEPROM flush is a dirty-flag check, enrollment-relay drain and route-report/beacon-relay timers are all `esp_timer_get_time()`-gated single comparisons — no loop, no delay.
- `Mesh::drain()`/`drainRecvQueue()`: bounded by ring-buffer contents (`xRingbufferReceive(..., 0)` — zero-tick, non-blocking); drains whatever is queued and returns.
- `Button::isPressed()`: rolling-vote debounce (Phase H2, item T) — samples at most once per `DEBOUNCE_DELAY_MS`, no blocking.
- `Led::pulse()`/`update()`: non-blocking state machine (Phase I Task 7, item WW) — the old `blink()`'s internal `delay()`-driven blocking is gone.
- `SevenSegDisplay` via `DisplayManager::tick()`: change-detection gate (`_lastValue`/`_lastNodeId`/`_lastIsMaster`/`_wasEnrolled`) — `display.show()`/`showWithDP()` only fire when rendered content actually changes, not every tick.
- `PirAdapter::loop()`: pure state-machine tick (`isMotionDetected()`/cooldown timer check) — the actual motion signal is a real hardware interrupt (`Pir::attachInterrupt()`), not polling.
- `SerialAdapter::loop()`: `uart_read_bytes(..., ticks_to_wait=0)` — non-blocking native UART read; health-report send is interval-gated.
- **RX path (this task's change):** ISR → `xRingbufferSendFromISR` + `vTaskNotifyGiveFromISR` → dedicated task blocks on `xTaskNotifyWait(portMAX_DELAY)` → wakes only on an actual received frame. No polling anywhere in this path anymore.

**Bounded, non-steady-state exceptions found (all pre-existing, all correctly scoped to rare/imminent-restart paths):**
- `ButtonHandler::pumpLedsUntilIdle()` (`while(true)` + `delay(5)`) and the `delay(2000)`/`delay(3000)` calls in `tickConfig`/`tickReset`: only reached immediately before a deliberate `ESP.restart()` (role toggle or EEPROM-wipe confirmation) — not part of normal steady-state operation, and each such event is followed by a full reboot, not a return to idle.
- `Error.h`'s `fatal()` `while(true) { delay(1); }` halt loop and `BootManager.h`'s WDT-loop escalation halt: both are terminal fault states, not steady-state.
- `SevenSegDisplay::init()`'s one-time `delay(500)` self-test: runs once at boot, before the scheduler's steady state matters.
- `Button::waitForHold(uint32_t ms)`: has a blocking `while(isPressed()) { delay(10); }` loop, but **grepped for all call sites in `firmware/main` and `tests` — it is never called anywhere.** Dead code; flagging for potential removal in a follow-up but it cannot affect current draw since it never runs.

**Conclusion:** with this task's change (RX-ISR → task-notify replacing the last remaining polled path — the recv-queue drain that used to run unconditionally as the first statement of every `loop()` iteration), there is no remaining code path in the steady-state hot loop that keeps the CPU busy or artificially wakes it outside of a real event (mesh RX, button press, PIR interrupt, or a timer genuinely due). This satisfies the brief's stated prerequisite ("Wake sources must be complete — any lingering polling loop defeats sleep") as far as static analysis can confirm.

**One architecturally significant caveat, worth flagging explicitly:** `main.cpp`'s `loop()` ends with an unconditional `delay(1)` (comment: "Yield to FreeRTOS idle task — allows CPU power gating between iterations"). This is the Arduino-compatibility loop task (still present pending Task 10's `app_main()` migration) blocking on `vTaskDelay(1 tick)` — at `CONFIG_FREERTOS_HZ=1000` that's a ~1ms period. This means the loop task itself is *guaranteed* to be rescheduled roughly every 1ms regardless of whether any real event occurred, which caps the maximum single light-sleep stretch tickless idle can grant at ~1ms — the system cannot coast through longer idle gaps (e.g., seconds between mesh beacons) the way it could if the loop task blocked on a longer or fully event-driven wait. Each 1ms sleep-entry/exit cycle still has fixed overhead (DFS ramp, sleep-entry/exit latency), so the *achievable* current reduction is bounded well below what an idealized "only wake on real events" design would get, purely because of this pre-existing (out of this task's scope — it's Arduino-loop-task plumbing, not mesh-specific) 1ms cadence. This is likely the largest single factor standing between the observed hardware result and the 30-40% target, and is a natural thing to revisit once Task 10 removes the Arduino loop wrapper.

## Current-measurement gate (plan Step 7) — SKIPPED

No flashable hardware was available in this session. The 25% minimum / 30-40% target current-draw reduction from the plan is **unverified locally**. Per the task instructions, this requires a post-merge deploy-and-measure pass by a maintainer with physical access to a leaf node and a multimeter (60s at-rest probe, compared against a pre-Task-9 baseline flash of the same hardware). The wake-source analysis above gives reasonable confidence the software side is correctly structured for tickless idle to help, but the actual magnitude — and whether the `delay(1)` caveat above meaningfully caps it below the target — can only be settled by that hardware measurement.

## Concerns

1. **`delay(1)`-capped sleep depth** (detailed above) — the dominant open question for whether 30-40% is achievable pre-Task-10.
2. **`setCpuFrequencyMhz(80)` / `esp_pm_configure()` interaction, undisturbed by design:** `main.cpp` still has its pre-existing (Phase-G-era) `if (!isMaster) setCpuFrequencyMhz(80);` call later in `setup()`, after this task's `esp_pm_configure(&pm_cfg)` (max 240 / min 80 / light sleep) runs earlier in the same function. arduino-esp32's `setCpuFrequencyMhz()` internally calls `esp_pm_configure()` again when `CONFIG_PM_ENABLE` is on, which could narrow or override the DFS range this task establishes for leaf nodes specifically (masters are unaffected — they never call it). Left untouched deliberately, since it's outside this task's stated scope (sdkconfig + `main.cpp`'s new PM config + the mesh task — not a redesign of the existing master/leaf frequency split), but it's worth a maintainer's look: at minimum it's redundant now that DFS auto-scales down to 80MHz when idle; at worst it could pin leaves at a fixed 80MHz with `light_sleep_enable` reset to whatever arduino-esp32's internal call defaults to, which would need verification against arduino-esp32's actual `setCpuFrequencyMhz()` source for this IDF/arduino-esp32 version pairing (not verified in this session — no hardware to confirm behaviorally, and reading through the vendored `managed_components/espressif__arduino-esp32` source was out of scope for a static-analysis-only task).
3. **`Button::waitForHold()` dead code** — noted above, harmless but worth a cleanup pass.
4. **Size cost is real and IRAM-heavy** (+8,244 B IRAM specifically) — flagged in the size-delta section; not a blocker (still 25% flash headroom) but worth tracking if a future task adds more IRAM-resident code (ISR handlers, etc.) on top.
5. `firmware/dependencies.lock`, `firmware/managed_components/`, `firmware/sdkconfig`, `firmware/build/` were generated by the ESP-IDF builds run in this worktree and were deleted before finalizing (not committed — same not-technically-gitignored situation flagged in the Task 8 report). Restated here since every task that runs a real `idf.py build` will keep hitting this until the `.gitignore` is fixed in a follow-up.
