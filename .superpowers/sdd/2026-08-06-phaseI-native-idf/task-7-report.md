# Task 7 Report: GPIO natives + LE memcpy + String elim + Led non-blocking (items MM+PP+QQ+RR+SS+TT+XX+WW)

## Status
**DONE**

## Branch / Commit
- Branch: `feat/phaseI-task7-gpio-natives` (base: `origin/docs/phaseI-native-idf` @ `ed5c948`, umbrella tip with Tasks 1+2+3+4+5+6+8 merged)
- Commit: `<see git log>` — `feat(phaseI/task7): GPIO natives + LE memcpy + String elim + Led non-blocking (items MM+PP+QQ+RR+SS+TT+XX+WW)`
- Pushed to `origin/feat/phaseI-task7-gpio-natives`. No PR opened (per instructions).

## Test results
- **Host: 296/296 pass** (`rm -f firmware/main/config/master_pubkey_pin.h && cd tests && cmake -B build . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ctest --test-dir build --parallel 1`).
- **ESP-IDF build: clean.** `idf.py build -- -j2` (env: `IDF_PATH=$HOME/esp/esp-idf`, IDF v5.5.1) succeeds with no errors. The only warning in the full build log — `Mesh.cpp:356: ignoring attribute 'section (".iram1.2")' because it conflicts with previous 'section (".iram1.0")'` — is pre-existing (present in the Task-1 baseline build too), unrelated to this task's edits. `firmware/main/lib/lattice-protocol` submodule was uninitialized in this fresh worktree (same recurring issue Tasks 4/5/6/8 flagged); ran `git submodule update --init --recursive` before the first build.
- Ran clang-format 18 (`/opt/homebrew/opt/llvm@18/bin/clang-format -i --style=file <touched files>`) before the final host-test + ESP-IDF verification pass — both re-run clean after formatting.

## A hang caught by the host test suite (worth flagging)

The first `gpio_mock.cpp` draft made `gpio_set_level()` write the level it was passed into the same `_mockDigitalPinState` array `gpio_get_level()`/`digitalRead()` read from — a reasonable-looking "make the mock round-trip" choice, but wrong: `tests/mocks/Arduino.h`'s pre-existing `digitalWrite()` mock deliberately does **not** touch `_mockDigitalPinState` (it only increments `_mockDigitalWriteCallCount`; only `setMockDigitalRead()` — a test-only helper — writes pin state). `SevenSegDisplay::writeByte()`'s ACK-wait loop writes the last data bit to DIO via `gpio_set_level()`, then flips DIO to input and polls `gpio_get_level()` for the chip's ACK inside a `while ((now - start) < 20ms)` loop. With the write-coupled mock, that read saw the just-written bit instead of the test's simulated (default-LOW) ACK line, and since the host's mock clock (`time_mock.h`) only advances via explicit `advanceMillis()` calls, the bounded 20ms wait busy-spun forever — `test_display_manager` pegged a core at 100% CPU for 4+ minutes before being caught (`RepeatedTickWithSameValueDoesNotRewriteDisplay`, the first test to actually exercise `SevenSegDisplay::setSegments()`). Fixed by making `gpio_set_level()` decouple exactly like `digitalWrite()` always did — comment left in `gpio_mock.cpp` explaining why the coupling is a trap. Re-ran full host suite clean after the fix (296/296, no hangs).

## Per-sub-item status

### MM — SevenSegDisplay digitalWrite/digitalRead → gpio_set_level/gpio_get_level: **DONE**
- All bit-bang call sites in `start()`/`stop()`/`writeByte()` converted. `delayMicroseconds(3)` (via the existing `tmDelay()` helper) preserved untouched — TM1637 timing unaffected.
- The ACK-read direction flip inside `writeByte()` (`pinMode(_dioPin, INPUT)` → sample → `pinMode(_dioPin, OUTPUT)`) is a genuine per-transaction runtime direction change, not a per-init `pinMode()` — kept as an explicit native call (`gpio_set_direction()` + `gpio_set_pull_mode(GPIO_PULLUP_ONLY)`, replacing the ESP32-Arduino-core-specific quirk where `digitalWrite(pin, HIGH)` on an `INPUT`-mode pin silently enables the internal pull-up) rather than folding into main.cpp's bundled output-group config.
- `init()`'s two `pinMode()` calls removed per RR (see below); its `digitalWrite(..., HIGH)` calls converted to `gpio_set_level(..., 1)`, kept (they set initial level, not pin mode).

### PP — Button::isPressed() → gpio_get_level: **DONE**
- Single call site (`digitalRead(_pin) == HIGH` → `gpio_get_level(static_cast<gpio_num_t>(_pin)) == HIGH`). `HIGH` macro still resolves via the transitively-included `<Arduino.h>` (unchanged elsewhere this task — full Arduino-framework retirement is Task 10).

### QQ — Pir attach/detach → gpio_isr_handler_add/remove: **DONE**
- `Pir::attachInterrupt(void (*isr)(), int mode)`'s **public signature is unchanged** (all callers — `PirAdapter.cpp` — needed zero changes). Internally: `gpio_set_intr_type()` (mode `RISING`/`FALLING` mapped to `GPIO_INTR_POSEDGE`/`GPIO_INTR_NEGEDGE`, else `GPIO_INTR_ANYEDGE`) + `gpio_isr_handler_add()` + `gpio_intr_enable()`. Detach: `gpio_intr_disable()` + `gpio_isr_handler_remove()`.
- **Signature adaptation needed:** native `gpio_isr_handler_add()`'s handler type is `void(*)(void*)`, not the zero-arg `void(*)()` `Pir::attachInterrupt()` accepts. Added a static `Pir::isrTrampoline(void* arg)` that `static_cast<Pir*>(arg)`s the `this` pointer passed as `arg` and forwards to a new private `_isrCallback` member — cleaner than casting the zero-arg function pointer itself to/from `void*` (which is only conditionally-supported by the standard), and keeps `PirAdapter`'s trampoline pattern (`detectMotionTrampoline`) completely unchanged.
- `gpio_install_isr_service(0)` is called once in `main.cpp::setup()`, before `adapter->init()` (which reaches `PirAdapter::init() → Pir::attachInterrupt()`).

### RR — bundled gpio_config_t (output + input groups): **DONE**
- `main.cpp::setup()` now has three `gpio_config_t` blocks, all before any component `init()` call:
  - **Output group:** `RED_LED_PIN`, `GREEN_LED_PIN`, `SEVSEG_DATA_PIN`, `SEVSEG_CLK_PIN` — `GPIO_MODE_OUTPUT`, no pulls.
  - **Button input group:** `CONFIG_BUTTON_PIN`, `RESET_BUTTON_PIN` — `GPIO_MODE_INPUT`, **pull-DOWN** (matches `Button::init()`'s prior `pinMode(_pin, INPUT_PULLDOWN)` — the codebase's buttons are active-HIGH-when-driven, not the brief's illustrative pull-up example).
  - **PIR input group:** `lattice::adapter::PIR_ADAPTER_DEFAULT_PIN` (27) — `GPIO_MODE_INPUT`, pull-UP (matches `GpioInput::init()`'s prior default `pinMode(_pin, INPUT_PULLUP)`). `AdapterFactory::getDefaultPinForAdapter()` always returns this same constant for `PIR_ADAPTER` regardless of EEPROM state, so it's safe to configure statically at boot even before the adapter type is known.
  - `gpio_install_isr_service(0)` immediately after (QQ).
- Removed the per-init `pinMode()` call from `Led.cpp`, `Button.cpp`, `GpioInput.cpp`, `GpioOutput.cpp`, `SevenSegDisplay.cpp`'s `init()` methods — each now only validates the pin (`isValidInputPin`/`isValidOutputPin`) and sets `_initialized = true`. Verified via `grep -rn "pinMode(" firmware/main/src` — zero remaining call sites (only comments).
- Also converted (not removed) `Led`'s destructor teardown `digitalWrite`/`pinMode` pair to `gpio_set_level`/`gpio_set_direction` — it's runtime teardown, not per-init setup, so it stays as an explicit native call. In practice this destructor never runs (LEDs are file-scope statics with program-lifetime duration on the real target), but it's kept correct.

### SS — E2ECrypto/RouteMac LE packing → memcpy: **DONE**
- `E2ECrypto.h::buildNonce`/`buildAad`, `RouteMac.h::buildHopContext` — every byte-by-byte `shift+mask+store` triplet/quad replaced with `memcpy(dst, &field, sizeof(field))`.
- `mesh_message` is `__attribute__((packed))`, so `memcpy` isn't just cosmetic here: `msg.epoch_num`/`msg.seq_num` aren't guaranteed 4-/2-byte aligned inside the packed struct, and a raw pointer-cast dereference (`*(uint32_t*)&msg.epoch_num`) risks a genuine unaligned-access fault on Xtensa. `memcpy` is well-defined for any alignment — the conversion is a correctness improvement, not purely stylistic.
- **Verification:** `tests/unit/test_route_mac.cpp`'s `buildHopContext` test `memcmp`s the output against a literal expected byte array (exact epoch/seq LE layout, `ASSERT_EQ(0, memcmp(...))`) — passed unchanged. `buildNonce`/`buildAad` don't have a standalone byte-pinned unit test, but every AEAD roundtrip/tamper/replay e2e test (`test_e2e_aead.cpp`, `TamperedFrameIsDropped`, `ForgedFrameWithoutValidTagIsDropped`, `SealedUplinkDeliversPlaintextToHub`, `ReplayedFrameIsDropped`, `SeqWrapBumpsEpochAndKeepsSealing`, etc. — all part of the 296) depends on nonce/AAD bytes being exactly correct for `crypto_aead_chacha20poly1305_ietf_{en,de}crypt` to round-trip and for tamper detection to still catch a modified frame; all pass unchanged. Wire bytes byte-for-byte identical, confirmed indirectly but comprehensively.

### TT — 4 straggler String() sites: **DONE**
- Brief's example line numbers (Mesh.cpp:72,298,424, EepromManager.cpp:187) had drifted post-Tasks-1-6/8 as flagged; located by grepping for `String(` directly. Actual 4 sites: `Mesh.cpp` (readMacAddress error, esp_now_init failure, esp_now_send failure — all `(String("...") + esp_err_to_name(x)).c_str()` fed into `lattice::err::fail`'s `const char*` param) and `EepromManager.cpp:399` (`logOperation("Peer list saved", String(numPeers).c_str())`).
- All 3 Mesh.cpp sites feed `lattice::err::fail(...)`, not a Logger call directly — converted each to a local `char errBuf[80]` + `snprintf` + pass `errBuf` (not `LATTICE_LOGF`, since the target isn't a Logger call; same string-elimination effect). EepromManager.cpp's site converted to a `char numPeersBuf[12]` + `snprintf("%zu", ...)`.
- Also found and fixed **3 additional non-macro `Logger::logln(..., String(...) + ...)` call sites in `main.cpp`** (not caught by the brief's `LATTICE_LOGLN`-only grep pattern, since `main.cpp` calls `Logger::logln` directly rather than through the macro) — these were mandatory to fix regardless, since the XX signature change makes them non-compiling, but worth noting as beyond the brief's literal 4-site count. All 3 collapsed to a ternary-of-two-literals (`isDevMode ? "Running in DEV mode" : "Running in PRODUCTION mode"`, etc.) — simpler than a buffer, since both branches were always fixed strings.
- `grep -rn "\bString\b" firmware/main/src firmware/main/main.cpp` now matches only comments describing the removal.

### XX — Logger signature change + caller migration: **DONE**
- `Logger::log`/`Logger::logln`: `const String&` → `const char*`, in both `Logger.h` and `Logger.cpp`. `Serial.print(const char*)`/`Serial.println(const char*)` already existed as overloads (both the real Arduino-ESP32 `Print` base and `tests/mocks/serial_mock.h`), so the implementation bodies needed no changes beyond the parameter type.
- Grepped every `LATTICE_LOGLN`/`LATTICE_LOG`/`Logger::log`/`Logger::logln` call site repo-wide (166 matches) — after the TT fixes above, zero remaining sites pass a `String`. `LOG_D`/`LATTICE_LOGF` macros (both already snprintf-into-`char[128]`-then-pass-`char*`) needed no changes — they were already `const char*`-shaped.
- **`MacAddress::toString()`** (sub-item): changed from `String toString() const` (heap-touching `sprintf` + `String` construction) to `void toString(char out[18]) const` (`snprintf` directly into a caller-provided 18-byte buffer — "aa:bb:cc:dd:ee:ff" + NUL). Grepped repo-wide (`grep -rn "toString(" firmware/main/src tests/`): **zero existing callers** anywhere in production or test code, so this was a pure signature change with no call-site migration needed — kept for future debug/log use, matching Logger's new char*-only contract.

### WW — Led non-blocking blink: **DONE**
- `Led::blink(times, onMs, offMs)` (blocking — internally called `delay()` `times` times) replaced with `Led::pulse(times, onMs, offMs)` (arms a state machine, returns immediately) + `Led::update(uint64_t nowMs)` (advances it — call every main-loop iteration) + `Led::isBusy() const` (convenience helper not in the brief's minimal sketch, added so callers that must wait for a pattern to finish can pump `update()` locally instead of the old internal blocking).
- State machine reproduces the exact old timeline: ON for `onMs`, OFF for `offMs`, repeated `times` times, **no trailing OFF wait after the final ON phase** (matches the old `if (i < times - 1) delay(offTimeMs)` guard) — verified by inspection against the removed loop, no dedicated Led unit test exists to assert against directly.
- **All 9 `blink()` call sites migrated** (`grep -rn "\.blink(" firmware/main` now empty):
  - `main.cpp::dataRecvCallback` (the actual motivating hot path — this used to block every ESP-NOW receive callback for ~300ms) and `main.cpp::loop()`'s startup blink: converted to bare `.pulse(...)`, relying on `loop()`'s new per-iteration `greenLed.update(nowMs)`/`redLed.update(nowMs)` calls (added right after `err_core::drainPendingBlink()`, before the unenrolled-node early `return`, so blinking never stalls pre-enrollment).
  - `main.cpp`'s red-LED-init-failure halt loop (`while(true) { blink(6,100,100); delay(1000); }`, never returns to `loop()`): converted to an inline pump (`pulse()` then a local `while(isBusy()) { update(); delay(10); }` loop, then `delay(1000)`) — the one case where the caller legitimately needs to keep blocking (nothing else runs on this path anyway).
  - `ButtonHandler.h`'s 4 sites: 2 (role-toggle dev-mode flip, reset-armed confirmation) fall through normally after arming — bare `.pulse(...)`. The other 2 are immediately followed by `delay(N); ESP.restart();` (visual confirmation before an intentional reboot) — added a small `pumpLedsUntilIdle(Led&, Led* = nullptr)` static helper that pumps `update()` in a local loop until the armed pattern finishes, preserving the pre-restart blink visually instead of just showing a solid-ON LED for the delay window.
  - `ErrorCore.cpp::blinkPattern()` (the shared error-LED pattern, reached from both `signalError()`'s synchronous path and `drainPendingBlink()`'s deferred-from-callback path): `.blink()` → `.pulse()`. Added a new `err_core::tick()` function (pumps `_state.errorLed->update(now)`) — called from `main.cpp::loop()` every iteration, **and** from `Error.h::fatal()`'s `while(true){}` halt loop (previously an empty spin — without pumping, the halted device would just show a solid-ON error LED instead of the intended blink pattern; the pump call plus a `delay(1)` were added there).

## Size Delta (`idf.py size`)

Baseline is this branch's own pre-edit build (base commit `ed5c948`, Task 7's Step 1).

| Section | Baseline (`ed5c948`) | Post-Task-7 | Delta |
|---|---:|---:|---:|
| .text (Flash Code) | 548,036 | 546,388 | **-1,648** |
| .rodata (Flash Data, incl. .appdesc) | 116,216 | 115,808 | **-408** |
| IRAM (.text + .vectors) | 92,787 | 92,787 | 0 |
| .bss | 21,800 | 21,360 | **-440** |
| .data | 16,196 | 16,196 | 0 |
| **Total image** | **773,267** | **771,211** | **-2,056** |

All four sub-totals moved in the expected direction for this task: `.text`/`.rodata` down from removing the Arduino GPIO wrapper indirection (MM/PP/QQ/RR) and String/heap machinery (TT/XX — fewer format-string/String-constructor code paths and their associated `.rodata`); `.bss` down from Led's new 5-member `pulse()`/`update()` state (small, a few bytes per instance) being more than offset elsewhere — likely `Logger`'s `String`-by-value parameter passing convention going away. Net -2,056 B, modest but consistent with a task whose primary goal was API-surface migration rather than large structural removal (that was Tasks 2/6/8).

## Concerns / Follow-ups
- **Host-mock hang (see above)** — not a production risk (the bug was entirely inside `tests/mocks/gpio_mock.cpp`, never shipped to firmware), but worth flagging for whoever writes the next ESP-IDF-native mock: any `gpio_set_level()`-style mock must stay decoupled from `gpio_get_level()`'s backing state unless a test explicitly opts into read-back via `setMockDigitalRead()`, exactly mirroring `digitalWrite()`/`digitalRead()`'s existing (deliberate) decoupling. A coupled mock will silently deadlock any code that writes then reads the same "pin" inside a bounded-but-mock-clock-driven wait loop — `SevenSegDisplay`'s TM1637 ACK read is exactly that pattern, and future GPIO-bitbang code will hit the same trap if this lesson isn't preserved.
- **WW's pre-restart pump helper (`ButtonHandler::pumpLedsUntilIdle`)** reproduces the old blocking-blink visual behavior faithfully for the 2 call sites that need it, but does so via an explicit local busy-wait — a deliberate, narrow exception to "Led no longer blocks internally," justified because these specific 2 sites are moments before an intentional `ESP.restart()` (nothing else is running that the blocking could starve). All other 7 `blink()`→`pulse()` conversions are genuinely non-blocking.
- **`Pir::isrTrampoline` uses `void* arg = this`** (object-pointer, not the zero-arg callback cast to `void*`) — a deliberate design choice to avoid the function-pointer↔`void*` conversion's conditionally-supported status in the C++ standard. Confirmed compiles clean on both AppleClang (host) and the xtensa-esp32 GCC toolchain (ESP-IDF build).
- `firmware/dependencies.lock`, `firmware/managed_components/`, `firmware/sdkconfig` were generated by the ESP-IDF build in this worktree and were **not** committed (same exclusion Tasks 6/8 flagged — not covered by `.gitignore`, excluded via explicit `git add <paths>` rather than `git add -A`).
