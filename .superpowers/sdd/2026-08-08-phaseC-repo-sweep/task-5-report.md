# Task 5 report: `ButtonHandler` dedup

**Status:** DONE

## What was done (picking up from disconnected prior implementer)

1. **Verified `detectHold()` + `tickConfig()` migration** (already in place from the
   prior implementer): correct as written, matches the brief's Step 1/2 exactly.

2. **Migrated `tickReset()`** (was NOT done — despite the file already containing code
   that looked like the brief's Step 3 pseudocode verbatim, tracing it against the
   original revealed it had the exact bug the brief's risk note warned about):
   the timeout check (`confirmPending && now > confirmDeadline`) ran whenever
   `detectHold()` returned `false`, which conflates "not pressed" with "pressed but
   still under HOLD_MS". The original gated the timeout check strictly on
   `!btn.isPressed()` (it lived in the `else` of `if (btn.isPressed())`). Concretely:
   holding the reset button continuously through a second hold cycle that crosses
   `confirmDeadline` before reaching the next `HOLD_MS` threshold would, under the
   buggy version, prematurely clear `confirmPending` mid-hold and let the eventual
   second hold-fire incorrectly re-arm a fresh confirm window instead of silently
   no-op'ing like the original.

   Fix: added an explicit `!btn.isPressed()` guard to the timeout check in
   `firmware/main/src/app/ButtonHandler.h`'s `tickReset()`. Verified by temporarily
   reverting the guard, rebuilding, and confirming the new regression test fails with
   exactly the predicted symptom, then restoring the fix and confirming it passes.

3. **Rewrote `tests/unit/test_button_handler.cpp`** from scratch using only verified
   real mock APIs (checked `tests/mocks/` and `tests/unit/test_button.cpp` first).
   Found and fixed several real problems in the prior draft:
   - `Button::getPin()` does not exist (only `Led` has `getPin()`) — replaced with
     known pin constants passed explicitly to test helpers.
   - `Mesh::getInstance()` returns `Mesh*`, not `Mesh&`, and is null until some `Mesh`
     is constructed — replaced with a local `Mesh mesh;` fixture member (matches
     `tests/unit/test_mesh_logic.cpp`'s own pattern).
   - `Button::isPressed()` debounces over 3 samples taken ≥5ms apart (real, verified
     against `Button.cpp`); a single call right after flipping the pin does not
     register — rewrote `pressButton()`/`releaseButton()` helpers to drive real
     debounce warmup with a leading `advanceMillis(5)` so they're correct regardless
     of prior poll timing.
   - **Pin 24** (used for `redLed` in the prior draft) is not a valid `GpioOutput`
     pin per `GpioOutput::isValidOutputPin()`'s mask — `Led::init()` silently failed,
     and the first `pulse()` call escalated through `lattice::err::fail()` into a
     thrown `ErrorCore::restartDevice` (UNIT_TEST build). Switched to pin 23.
   - **Found and fixed a real test-hang risk**: `tickConfig`/`tickReset` keep their
     hold-tracking state (`wasPressed`/`holdStart`/`confirmPending`/`confirmDeadline`)
     as **function-local `static`s**, which persist across every `TEST_F` in the
     binary (pre-existing production design, not something this task changes). A
     prior test's leftover `confirmPending==true` + stale `confirmDeadline` made a
     later test's very first hold-fire land in the confirmed-wipe branch, which calls
     `esp_restart()` via `pumpLedsUntilIdle()` — a blocking loop keyed off
     `esp_timer_get_time()` that the host mock clock (which only advances via
     explicit `advanceMillis()`) can never satisfy, since `vTaskDelay` is a no-op on
     host. This hung the test binary for real (confirmed via a 20-30s `alarm`-guarded
     run). Fixed by adding a `quiesceButtonHandlerStatics()` step to `SetUp()` that
     jumps the mock clock far ahead, ticks once with both buttons unpressed (forces
     `wasPressed=false` on both functions and clears any stale `confirmPending` via
     the ordinary, safe timeout path — never the confirmed-wipe path), then resets
     the clock to 0 for the actual test.
   - Every test in the file is designed to never reach the confirmed-wipe /
     production-role-toggle branches (both end in the same blocking
     `pumpLedsUntilIdle()` pattern) — this is a deliberate, documented limitation
     (see file-level comment), not an oversight. Where LED-pulse state needed to be
     observed as a proxy for internal (non-observable) `confirmPending` state, tests
     drain `Led::update()` with synthetic timestamps rather than the shared mock
     clock, to avoid perturbing the button-hold timeline under test.

   Final 6 tests, all passing:
   - `ConfigButtonDoesNotFireBeforeHoldThreshold`
   - `ConfigButtonFiresExactlyAtHoldThreshold`
   - `ConfigButtonDoesNotRefireAfterRelease`
   - `ResetButtonFirstHoldArmsConfirmWindow`
   - `ResetArmsAgainAfterAProperTimeout` (brief's Step 4 a/b)
   - `ResetTimeoutDoesNotFireWhileButtonIsContinuouslyHeld` — the critical regression
     test for the risk note (brief's Step 4 c-equivalent coverage, adapted since the
     literal "hold again within window → clearAll()/esp_restart()" path is
     untestable on this host harness without hanging; see above). **Verified this
     test actually catches the bug**: reverting the `!btn.isPressed()` guard makes it
     fail with `redLed->isBusy()` unexpectedly `true` (a bogus re-arm), exactly as
     predicted.

4. Registered the new test in `tests/CMakeLists.txt` (`add_unit_test(test_button_handler unit/test_button_handler.cpp)`).

5. Ran `/opt/homebrew/opt/llvm@18/bin/clang-format` on both changed source files;
   `ButtonHandler.h` was already clean, `test_button_handler.cpp` needed one
   reformat, applied.

6. Full build + test run:
   - `cmake --build tests/build --parallel 2` — success.
   - `ctest --test-dir tests/build --output-on-failure --label-exclude e2e` —
     **295/295 passed**.
   - `ctest --test-dir tests/build --output-on-failure --label-regex e2e` —
     **41/41 passed**.

## Constraints honored

- Firmware-only change; no wire-format changes.
- `ButtonHandler::tick(...)`'s public signature unchanged (verified via `git diff`).
- Only `firmware/main/src/app/ButtonHandler.h`, `tests/unit/test_button_handler.cpp`,
  and `tests/CMakeLists.txt` (build registration for the new test) touched.

## Commit

Committed on branch `task-5-button-hold-dedup`, on top of `4120641`. Not pushed, not
merged, per instructions.
