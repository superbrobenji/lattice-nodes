# Task 3 Report — `docs/adapter_development_guide.md` rewrite

**Status:** DONE

**Commit:** (see controller report) on branch `phaseD-task3-manual` —
`docs(phaseD): rewrite adapter_development_guide.md for current Adapter API`

## What was done

Full rewrite of `docs/adapter_development_guide.md`, replacing the stale
Arduino-era doc (capitalized `src/Adapter/PIR_Adapter/` directory convention,
`Adapter(adapter_types type, int pin)` constructor, `LED_ADAPTER`/`WIFI_ADAPTER`
enum values, `arduino-cli`/`main.ino` build workflow — none of which exist in
the current tree) with an accurate current-state guide, per the brief's outline:

1. **Table of Contents** — kept the old doc's four sections but reordered so
   Architecture Overview comes first (readers need the base-class API before
   the walkthrough makes sense), matching the brief's outline order.
2. **Adapter Architecture Overview** — `adapter_types` enum (only
   `UNKNOWN_ADAPTER=0, SERIAL_ADAPTER=1, PIR_ADAPTER=2`, with an explicit note
   that no `LED_ADAPTER` exists and a footnote distinguishing the firmware
   enum from the separate, unrelated wire-protocol `ADAPTER_TYPE_LED`/`_RELAY`
   constants in the vendored `lattice-protocol` C header); full `Adapter` base
   class API (`explicit Adapter(uint8_t pin)` — type is NOT a ctor param);
   `onMeshData`/`onMeshDataImpl` filtering semantics including the
   `SerialAdapter`-receives-everything-unfiltered exception; the shared
   control-op dispatch table (`opConfigSet`/`opNodeIdSet`/`opHealthReq`/
   `opTxPowerSet`); the health-report builder methods.
3. **Adding a New Adapter** — the research's 10-step walkthrough, transcribed
   with `PirAdapter` as the running example and a hypothetical `MyNewAdapter`
   as the worked example: lowercase directory (`src/adapter/mynew/`, no
   `Adapter/` parent, no underscore), header/constructor/`init()`/`loop()`
   patterns, enum registration, `AdapterFactory.h` default-pin constant +
   `AdapterFactory.cpp` factory-switch + include registration, GPIO boot
   config (centralized in `main.cpp`'s `initDrivers()`, not per-component
   `pinMode()`), and EEPROM persistence (all handled by the shared
   `Adapter::opConfigSet`, no per-adapter code).
4. **Changing the Default Adapter** — `DEFAULT_ADAPTER` in
   `firmware/main/project_config.h` (confirmed the constant name and current
   value `SERIAL_ADAPTER` directly from source), plus the two other paths
   (runtime `OP_CONFIG_SET` opcode; reset-button EEPROM wipe via
   `lattice::eeprom::clearAll()`, confirmed against `ButtonHandler::tickReset()`
   — 5s hold to arm, 3s window to confirm).
5. **Testing Your New Adapter** — host-test pattern via
   `cmake -B tests/build tests/ ... && ctest ...` (cross-checked against
   `README.md`'s "Running Unit Tests" section for the exact commands), citing
   `tests/unit/test_pir_adapter.cpp` as the pattern to copy (confirmed it
   exists and read its actual test bodies — `PIRHealthTest::
   SendsNodeHealthAfter30s`/`DoesNotSendNodeHealthBefore30s` — to describe
   real assertions rather than invented ones), plus the `tests/CMakeLists.txt`
   `add_unit_test(...)` registration step and a common-issues table.

## Verification performed

- `find firmware/main/src/adapter -type f` — confirmed current layout is
  `adapter/Adapter.{h,cpp}`, `adapter/AdapterFactory.{h,cpp}`,
  `adapter/pir/PirAdapter.{h,cpp}`, `adapter/serial/SerialAdapter.{h,cpp}` —
  no `PIR_Adapter/` anywhere.
- Read `Adapter.h` in full: confirmed the `adapter_types` enum values,
  `explicit Adapter(uint8_t pin)` ctor signature, `onMeshData`/
  `onMeshDataImpl` split, health-report builder signatures, and the
  `ControlOpEntry`/`kControlOps` dispatch table declaration.
- Read `AdapterFactory.h`/`.cpp` in full: confirmed
  `PIR_ADAPTER_DEFAULT_PIN=27`/`SERIAL_ADAPTER_DEFAULT_PIN=255` live in
  `AdapterFactory.h` (not `project_config.h`), the `createAdapter` switch
  contents, and `adapterTypeFromEEPROM`'s `0xFF → PIR_ADAPTER` fallback.
- Read `PirAdapter.h` in full and the top of `PirAdapter.cpp`: confirmed the
  constructor pattern (`Adapter(pin), _pir(pin), ...` then
  `_adapterType = adapter_types::PIR_ADAPTER;` in the body).
- Read `Adapter.cpp`'s `opConfigSet` (confirmed wire format
  `[0xC1][6B targetMac][1B adapterType]` and the `esp_restart()` call).
- Read `firmware/main/project_config.h` lines 20-65: confirmed
  `DEFAULT_ADAPTER` constant name/type/current value and the `DEV_MODE`
  comment.
- Read `firmware/main/main.cpp`'s `initSubsystems()`: confirmed
  `DEFAULT_ADAPTER` is only consulted in the `isDevMode` branch, and
  `createFromEEPROM()`/`initializeDefaultsIfUnset()` (which seeds
  `PIR_ADAPTER`, not `DEFAULT_ADAPTER`) run otherwise — this contradicted my
  first draft's assumption and I corrected the doc to say `DEFAULT_ADAPTER`
  only applies in `DEV_MODE`.
- **Caught and fixed one inaccuracy inherited from the old doc**: the old doc
  referenced `EEPROM_Manager::getInstance().clearAll()`. Grepped for
  `EEPROM_Manager` in the current tree — no such class exists. The real call
  is the free function `lattice::eeprom::clearAll()`
  (`firmware/main/src/persistence/eeprom/EepromCore.{h,cpp}`), invoked from
  `ButtonHandler::tickReset()` (`firmware/main/src/app/ButtonHandler.h`),
  which I read directly to confirm the 5s-hold/3s-confirm timing
  (`HOLD_MS = 5000`, `confirmDeadline = now + 3000`). Rewrote that section to
  cite the real function and file.
- Confirmed `tests/unit/test_pir_adapter.cpp` exists (`find` + full read of
  its first ~80 lines) and used its actual test names/assertions
  (`captureTransmit`, `PIRHealthTest`, the exact health-frame byte offsets)
  rather than paraphrasing from the research alone.
- Cross-checked `README.md`'s "Running Unit Tests" section for the exact
  `cmake`/`ctest` invocation used in the new doc's testing section.
- Confirmed `tests/CMakeLists.txt` lists `add_unit_test(test_pir_adapter
  unit/test_pir_adapter.cpp)` and that `FIRMWARE_SOURCES` individually lists
  `adapter/Adapter.cpp`, `adapter/AdapterFactory.cpp`,
  `adapter/pir/PirAdapter.cpp`, `adapter/serial/SerialAdapter.cpp` — used
  this to justify the "add your adapter's `.cpp` to `FIRMWARE_SOURCES`"
  common-issue entry.
- Confirmed `lattice::eeprom::loadAdapterType()`/`saveAdapterType()` exist in
  `EepromDeviceConfig.h` under exactly those names.

## Self-assessment

Factual accuracy: every code snippet, constant name, and file path in the
new doc was verified by directly reading the corresponding source file in
this worktree during this session (not solely transcribed from the research
file, though the research's 10-step walkthrough was accurate everywhere it
was checked and is the doc's structural backbone). The one place my first
draft would have shipped a factual error — citing a nonexistent
`EEPROM_Manager::getInstance().clearAll()` API carried over by habit from the
old doc's phrasing — was caught by grepping the current tree before
finalizing, not by the research file (which correctly used
`lattice::eeprom::clearAll()` terminology but the old doc's exact wording
slipped into an early draft anyway).

## Concerns

None blocking. One judgment call worth flagging for the controller: the doc
states the standard ESP-IDF hardware-verification command
(`idf.py build flash monitor`, run from `firmware/`) in the final "Testing"
paragraph. This is standard ESP-IDF usage and matches the `idf.py`
invocation pattern used elsewhere in this repo's planning docs
(`docs/superpowers/plans/2026-08-06-phaseJ-crypto-revert.md`), but no
top-level doc (`README.md`/`CONTRIBUTING.md`) currently documents the
flash/monitor command explicitly, so this line is inference from ESP-IDF
convention rather than a direct repo citation.
