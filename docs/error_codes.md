# Error Codes

This document is the registry for the firmware's error-code system: how a code is built, the
current public API for raising one, every real call site that raises one today, and how a code
shows up on hardware (the TM1637 seven-segment display and the error LED's blink pattern).

Related docs: `REFACTORING_GUIDE.md` (module map / architecture overview) links back here for the
error-code registry.

## 1. How codes work

Every error code is a **3-digit decimal `TMS` code**, built by `makeErrorCode()`
(`firmware/main/src/error/ErrorCodes.h`):

```cpp
uint16_t code = makeErrorCode(t, m, sub);
// == static_cast<uint16_t>(t) * 100 + static_cast<uint16_t>(m) * 10 + (sub % 10)
```

- **T — `ErrorTypeDigit`** (the error's *category*):

  ```cpp
  enum class ErrorTypeDigit : uint8_t {
    GENERIC  = 1,
    SENSOR   = 2,
    COMM     = 3,
    MEMORY   = 4,
    HARDWARE = 5,
    CONFIG   = 6,
    CRYPTO   = 7,   // AEAD/ECDH failures — e.g. AEAD_EPOCH_ROLLBACK
  };
  ```

- **M — `ModuleDigit`** (the *subsystem* that raised it):

  ```cpp
  enum class ModuleDigit : uint8_t { CORE = 1, ADAPTER = 2, MESH = 3, EEPROM = 4, HW = 5 };
  ```

- **S — sub-code** (`uint8_t`, module-specific qualifier). **Note the `% 10`:** `sub` is taken
  modulo 10 before it's folded into the code, so a literal sub-code of `25` displays as digit `5`,
  not as `25`. Two real call sites in the registry below do this on purpose (`E2ECrypto.h:21` and
  `:44`, sub-codes `25`/`26`, chosen so their source-level constants stay distinguishable from a
  companion set of LMK-path codes `10-19` even though only the low digit ever reaches the display —
  see the comment at `E2ECrypto.h:17`).

Example: `t=CONFIG(6), m=HW(5), sub=1` → `code = 600 + 50 + 1 = 651`.

Both `ErrorTypeDigit` and `ModuleDigit` are defined in `firmware/main/src/error/ErrorCodes.h`.

## 2. Current public API

`firmware/main/src/error/Error.h`, `namespace lattice::err`:

```cpp
inline bool fail(::lattice::core::ErrorTypeDigit t, ::lattice::core::ModuleDigit m,
                 uint8_t sub, const char* msg);

[[noreturn]] inline void fatal(::lattice::core::ErrorTypeDigit t, ::lattice::core::ModuleDigit m,
                               uint8_t sub, const char* msg);
```

Both take the same four arguments. `fail()` logs `msg` via `LATTICE_LOGLN` and calls
`err_core::signalError(t, m, sub, msg)` (drives the display/LED — see §5), then **returns `false`**
so the caller can keep going in a degraded state — **except when `t` is `MEMORY`, `HARDWARE`, or
`CRYPTO`.** For those three T-digit categories, `signalError(ErrorTypeDigit, ...)` maps (or, for
`CRYPTO`, *folds*) onto an `ErrorType` for which `shouldRestart()` is true (see §5), so
`err_core::signalError()` calls `restartDevice()` before ever returning control to `fail()`'s
caller — under `UNIT_TEST` that throws `lattice::err::FatalError`; on real hardware it calls
`esp_restart()` and the device reboots. In the §3 registry, 13 of the `fail()` call sites (11 in
`firmware/main/src` + 2 in `main.cpp`) have T = MEMORY, HARDWARE, or CRYPTO and so actually behave
this way rather than returning `false` — only the remaining `GENERIC`/`SENSOR`/`COMM`/`CONFIG`-tier
`fail()` calls genuinely let the caller continue in a degraded state. `fatal()` does the same
signaling, then **never returns** regardless of `t`: under `UNIT_TEST` it throws `FatalError`; on
real hardware it loops forever calling `err_core::tick()` + `vTaskDelay(1ms)` (keeps the LED blink
pattern animating instead of freezing solid-on).

There are two thin convenience wrappers, also in `Error.h`, that funnel through the same `fail()`:

```cpp
err::check(bool condition, utils::ErrorType type, const char* msg);
err::checkEsp(esp_err_t err, utils::ErrorType type, const char* msg);
```

Both resolve to `fail(toDigit(type), ModuleDigit::CORE, 0, msg)` — module is always `CORE` and
sub-code is always `0`. Live call sites: `mesh/PeerRegistry.cpp:179` (`checkEsp`,
`COMMUNICATION_FAIL`, "removePeerFromEEPROM: del_peer failed" → code **310**),
`mesh/MeshTransport.cpp:39` (`checkEsp`, `HARDWARE_FAILURE`, "Failed to set WiFi channel" → code
**510**), `mesh/MeshTransport.cpp:54` (`checkEsp`, `HARDWARE_FAILURE`, "Failed to set ESP-NOW PMK"
→ code **510**), `mesh/MeshTransport.cpp:199` (`checkEsp`, `COMMUNICATION_FAIL`,
"registerPeerWithEspNow: add_peer failed" → code **310**).

**There is no other public overload.** An older two-argument form,
`fail(utils::ErrorType, const char* msg)`, existed in this codebase historically but has been
**fully removed** — it is not available to call. `Error.h`'s own comment (lines 58-65) notes its
few call sites were migrated onto the digit-based `fail()` shown above (see `main.cpp`'s
`initHardwareOutputs()` in the registry below); the only surviving trace of the old `utils::ErrorType`
enum is the internal `toDigit(utils::ErrorType)` helper that lets `check()`/`checkEsp()` route
through the one real `fail()`. Do not call `err::fail(SomeErrorType, "message")` with a bare
two-argument form — it will not compile.

## 3. Registry — every call site

Grepped exhaustively against `firmware/main/src` (28 call sites) plus `main.cpp` (7 more — the
firmware's only other call sites, and the ones `Error.h`'s own comment above refers to). A
`ReplayCache.h:139` grep hit is a code *comment* referencing the real call at line 159, not a
second call site, and is excluded.

### `firmware/main/src` (28 call sites)

| # | File:Line | Call | T, M, S | Code | Message | Trigger |
|---|---|---|---|---|---|---|
| 1 | `mesh/Mesh.cpp:43` | `fail` | HARDWARE(5), MESH(3), 1 | **531** | "MESH: Failed to read MAC address: %s" | `esp_wifi_get_mac()` fails in `Mesh::readMacAddress()` |
| 2 | `mesh/ReplayCache.h:159` | `fail` | CRYPTO(7), MESH(3), 1 | **731** | "AEAD epoch rollback — refusing seal" | Seal-time nonce-reuse guard: new (epoch,seq) doesn't strictly advance past the last sealed frame |
| 3 | `mesh/PeerRegistry.cpp:154` | `fail` | MEMORY(4), MESH(3), 2 | **432** | "Peer list full! Cannot add new peer. MAX_PEERS reached." | Adding a peer when `peerCount >= MAX_PEERS` |
| 4 | `mesh/MasterBeacon.cpp:120` | `fail` | CONFIG(6), MESH(3), 7 | **637** | "Multiple master nodes detected! Network split or misconfiguration likely." | Second/duplicate master beacon detected on the mesh |
| 5 | `mesh/MeshCrypto.h:28` | `fatal` | CONFIG(6), MESH(3), 3 | **633** | "MESH: keypair gen failed" | `lattice::crypto::x25519_keygen()` fails inside `generateKeypair()` |
| 6 | `mesh/MeshTransport.cpp:50` | `fail` | COMM(3), MESH(3), 3 | **333** | "MESH: esp_now_init failed: %s" | `esp_now_init()` fails |
| 7 | `mesh/MeshTransport.cpp:161` | `fail` | COMM(3), MESH(3), 5 | **335** | "MESH: Error sending message: %s" | `esp_now_send()` returns an error |
| 8 | `mesh/E2ECrypto.h:21` | `fatal` | CONFIG(6), MESH(3), 25→**5** | **635** | "MESH: computeSharedSecret — x25519 failed" | `lattice::crypto::x25519_shared()` fails in `computeSharedSecret()`. Sub-code literal is `25`; `makeErrorCode` mods by 10, so the displayed/registry digit is `5` |
| 9 | `mesh/E2ECrypto.h:44` | `fatal` | CONFIG(6), MESH(3), 26→**6** | **636** | "MESH: deriveE2EKeys — hkdf failed" | HKDF derivation fails in `deriveE2EKeys()`. Same `sub % 10` note (literal `26` → displayed `6`) |
| 10 | `hardware/output/SevenSegDisplay.cpp:44` | `fail` | CONFIG(6), HW(5), 1 | **651** | "7Seg invalid pins" | `init()` called with invalid DIO/CLK GPIO pins |
| 11 | `hardware/output/SevenSegDisplay.cpp:127` | `fail` | HARDWARE(5), HW(5), 2 | **552** | "7Seg ACK timeout" | TM1637 ACK bit not received during a write |
| 12 | `hardware/output/Led.cpp:32` | `fail` | CONFIG(6), HW(5), 1 | **651** | "Led: Invalid pin number" | `GpioOutput::init()` fails (invalid pin). **Collides with row 10.** |
| 13 | `hardware/output/Led.cpp:48` | `fail` | HARDWARE(5), HW(5), 2 | **552** | "Led: on() called before initialization" | `Led::on()` called on an uninitialized LED. **Collides with row 11.** |
| 14 | `hardware/output/Led.cpp:60` | `fail` | HARDWARE(5), HW(5), 3 | **553** | "Led: off() called before initialization" | `Led::off()` before init |
| 15 | `hardware/output/Led.cpp:72` | `fail` | HARDWARE(5), HW(5), 4 | **554** | "Led: toggle() called before initialization" | `Led::toggle()` before init |
| 16 | `hardware/output/Led.cpp:115` | `fail` | HARDWARE(5), HW(5), 5 | **555** | "Led: pulse() called before initialization" | `Led::pulse()` before init |
| 17 | `adapter/pir/PirAdapter.cpp:29` | `fail` | CONFIG(6), ADAPTER(2), 1 | **621** | "PIR_Adapter: PIR hardware failed to initialize." | `_pir.init()` fails in `PirAdapter::init()` |
| 18 | `adapter/pir/PirAdapter.cpp:36` | `fail` | CONFIG(6), ADAPTER(2), 2 | **622** | "PIR_Adapter: Failed to attach interrupt." | `_pir.attachInterrupt()` fails during init |
| 19 | `adapter/pir/PirAdapter.cpp:103` | `fail` | HARDWARE(5), ADAPTER(2), 2 | **522** | "PIR_Adapter: Could not re-attach interrupt (possible hardware error)" | Re-attach after motion detection fails. Same sub `2` as row 18, but T digit differs (CONFIG vs HARDWARE), so the codes (622 vs 522) don't collide |
| 20 | `adapter/Adapter.cpp:37` | `fail` | CONFIG(6), ADAPTER(2), 1 | **621** | "Adapter: Transmit function not set" | `send()` called with no `mesh_transmit_fn` registered. **Collides with row 17.** |
| 21 | `adapter/serial/SerialFraming.cpp:173` | `fail` | COMM(3), ADAPTER(2), 2 | **322** | "Serial_Adapter: Invalid frame length" | Framed length byte is `0` or exceeds `MAX_PAYLOAD` |
| 22 | `adapter/serial/SerialFraming.cpp:188` | `fail` | COMM(3), ADAPTER(2), 3 | **323** | "Serial_Adapter: Frame buffer overflow" | `frameIndex >= MAX_PAYLOAD` while awaiting payload bytes |
| 23 | `adapter/AdapterFactory.cpp:34` | `fail` | CONFIG(6), ADAPTER(2), 2 | **622** | "AdapterFactory: Unknown adapter type" | `createAdapter()`/`createFromEEPROM()` given an unrecognized adapter-type enum. **Collides with row 18.** |
| 24 | `adapter/serial/SerialAdapter.cpp:104` | `fail` | COMM(3), ADAPTER(2), 4 | **324** | "Serial_Adapter: Message encoding failed" | Protobuf encode of an outgoing mesh message fails |
| 25 | `adapter/serial/SerialAdapter.cpp:194` | `fail` | COMM(3), ADAPTER(2), 5 | **325** | "Serial_Adapter: Failed to decode protobuf frame" | Incoming serial frame fails protobuf decode |
| 26 | `adapter/serial/SerialAdapter.cpp:254` | `fail` | CONFIG(6), ADAPTER(2), 6 | **626** | "Serial_Adapter: transmit function not set" | Serial adapter forwarding a broadcast command with no transmit fn registered |
| 27 | `persistence/eeprom/EepromCore.cpp:51` | `fail` | MEMORY(4), EEPROM(4), 5 | **445** | "NVS write failed (security-relevant key)" | `persistOrEscalate()` — a write to a security-relevant NVS key (mesh key, keypair, etc.) wrote fewer bytes than expected |
| 28 | `persistence/eeprom/EepromCore.cpp:208` | `fail` | MEMORY(4), EEPROM(4), 1 | **441** | "EepromCore: NVS begin failed" | `nvs_open()` fails during `eeprom::init()` |

### `main.cpp` (7 more call sites — `initHardwareOutputs()` / `initSubsystems()`)

`main.cpp` lives outside `firmware/main/src`, but its 7 `err::fail`/`err::fatal` calls are the only
other call sites in the firmware, and are the ones `Error.h`'s own comment refers to as "migrated
to the digit-based fail() at their only 2 call sites" (the *legacy-overload* migration; there are 7
digit-based calls here in total, not 2 — see individual rows).

| File:Line | Call | T, M, S | Code | Message | Trigger |
|---|---|---|---|---|---|
| `main.cpp:363` | `fatal` | HARDWARE(5), CORE(1), 1 | **511** | "MAIN: Failed to initialize green LED" | `greenLed.init()` fails |
| `main.cpp:370` | `fail` | HARDWARE(5), CORE(1), 5 | **515** | "Config button init failed!" | `configButton.init()` fails |
| `main.cpp:376` | `fail` | HARDWARE(5), CORE(1), 6 | **516** | "Reset button init failed!" | `resetButton.init()` fails |
| `main.cpp:387` | `fatal` | MEMORY(4), CORE(1), 2 | **412** | "EEPROM Manager init failed!" | `lattice::eeprom::init()` (second, authoritative/checked call) fails |
| `main.cpp:440` | `fatal` | HARDWARE(5), CORE(1), 3 | **513** | "MAIN: Failed to create PIR adapter" | Adapter factory returns null |
| `main.cpp:447` | `fatal` | HARDWARE(5), CORE(1), 4 | **514** | "MAIN: Adapter failed to initialize" | `adapter->init()` fails |
| `main.cpp:459` | `fatal` | COMM(3), MESH(3), 1 | **331** | "MAIN: Mesh init failed — cannot operate without mesh" | `mesh.init()` fails |

That's 28 + 7 = **35 call sites** total.

## 4. Known code collisions

**This is real, current behavior of the shipped code — not a doc error.** Several distinct call
sites produce the identical 3-digit code today:

- **621** — `adapter/pir/PirAdapter.cpp:29` (PIR hardware init failure) **and**
  `adapter/Adapter.cpp:37` (transmit function not set)
- **622** — `adapter/pir/PirAdapter.cpp:36` (interrupt attach failure) **and**
  `adapter/AdapterFactory.cpp:34` (unknown adapter type)
- **651** — `hardware/output/SevenSegDisplay.cpp:44` (invalid display pins) **and**
  `hardware/output/Led.cpp:32` (invalid LED pin)
- **552** — `hardware/output/SevenSegDisplay.cpp:127` (TM1637 ACK timeout) **and**
  `hardware/output/Led.cpp:48` (LED used before init)

A reader decoding, say, "621" off the seven-segment display cannot tell whether it's a PIR
init failure or an unset transmit callback from the code alone — the numeric code doesn't fully
disambiguate. The only thing that distinguishes them is the log message passed to `fail()`
(e.g. "PIR_Adapter: PIR hardware failed to initialize." vs. "Adapter: Transmit function not set"),
and by default that message is **compiled out**: `LATTICE_LOGLN` folds to `((void)0)` when
`LATTICE_DEFAULT_LOG_LEVEL` is `LATTICE_LOG_LEVEL_NONE` (the `project_config.h` default), so on a
production build there is genuinely no way to tell these apart remotely — only the on-device
7-segment code, full stop. If you need to disambiguate a collision in the field, temporarily raise
the log level and reproduce, or narrow by which subsystem could plausibly be active at the time.

When adding a new code (§6), check this table (and the full registry in §3) for an existing
`T,M,S` combination before reusing one — collisions are not actively prevented by the code, only by
programmer discipline.

## 5. How a code reaches the hardware

### Seven-segment display (TM1637)

`err_core::signalError(t, m, sub, msg)` (`firmware/main/src/error/ErrorCore.cpp`) computes
`code = makeErrorCode(t, m, sub)` — the same `TMS` value from §1 — and calls
`_state.display->show(static_cast<int>(code))`. `SevenSegDisplay::show(int value, bool
leadingZeros = true)` zero-pads by default, so on the 4-digit display code `531` renders as
`0531`: **leftmost digit always blank/`0`, then T, then M, then S, left to right.**

If the display is compiled out (`ENABLE_SEVSEG_DISPLAY` off, so `_state.display == nullptr`), no
digits are shown at all — only the LED blink pattern below fires.

### Error LED blink pattern (coarser, separate signal)

`signalError` *also* maps the digit-based `t` (`ErrorTypeDigit`) down to the legacy coarse
`ErrorType` enum to drive the error LED's blink-count pattern via `blinkPattern()`
(`Led::pulse(count, 200, 200)`). This is a different — and coarser — signal than the 7-segment
digits, useful when the display itself is disabled or unreadable:

| `ErrorTypeDigit` | Blink count |
|---|---|
| HARDWARE | 6 |
| COMM | 3 |
| MEMORY | 4 |
| CONFIG | 5 |
| CRYPTO | 6 (mapped to the same class as HARDWARE — an AEAD epoch-rollback is treated as security-critical enough to halt like a hardware fault) |
| GENERIC, SENSOR (and anything else) | 1 (default fallthrough) |

`shouldRestart()` (`ErrorCore.cpp`) checks the coarse `ErrorType`, not the original
`ErrorTypeDigit`, and returns true only for `ErrorType::MEMORY_ERROR` and
`ErrorType::HARDWARE_FAILURE`. Because `signalError(ErrorTypeDigit, ...)` folds `CRYPTO` into
`ErrorType::HARDWARE_FAILURE` *before* that check runs (same fold used for the blink pattern
above), the effective set of T-digit categories that trigger `esp_restart()` after signaling is
**MEMORY, HARDWARE, and CRYPTO** — e.g. the `ReplayCache.h:159` AEAD epoch-rollback guard (code
**731**, `CRYPTO`) does reboot the device, not just blink. `GENERIC`, `SENSOR`, `COMM`, and
`CONFIG` do not restart.

`err_core::tick()` pumps `Led::update()` every main-loop iteration (and inside `fatal()`'s halt
loop, so the LED keeps animating even while halted) since `Led::pulse()` is non-blocking.

## 6. Adding a new code

Always go through `lattice::err::fail()`/`fatal()` — never call `err_core::signalError()` directly.

```cpp
#include "src/error/Error.h"

// Example: mesh peer-list overflow (T=MEMORY, M=MESH, sub=2) — this is the
// real call in mesh/PeerRegistry.cpp:154, code 432.
if (peerCount >= MAX_PEERS) {
  lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::MESH,
                     2, "Peer list full! Cannot add new peer. MAX_PEERS reached.");
  return false;
}
```

Before picking a sub-code:

1. Choose the `ErrorTypeDigit` (category) and `ModuleDigit` (subsystem) that already match your
   call site — do not invent a new enumerator without updating `ErrorCodes.h` and this doc together.
2. Pick a `sub` that keeps the resulting `T,M,S` code **distinct from every row in §3** if you
   reasonably can — remember `sub` is taken mod 10 (§1), so `sub=1` and `sub=11` produce the same
   code. §4's existing collisions are tolerated historical debt, not a pattern to extend.
3. Use `fail()` if the caller can continue in a degraded state, `fatal()` if continuing would be
   unsafe (`fatal()` never returns).
4. **Add a row to the §3 registry table in this doc** with the file:line, T/M/S, resulting code,
   message, and trigger — this table is the whole value of this document, and it goes stale the
   moment a new call site isn't transcribed here.
