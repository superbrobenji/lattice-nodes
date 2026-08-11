# Task 4 Report: nvs_flash direct (item CC + opportunistic UU)

## Status
**DONE**

## Branch / Commit
- Branch: `feat/phaseI-task4-nvs-flash` (base: `origin/docs/phaseI-native-idf` @ `a6f8070`)
- Commit: `5409719` — `feat(phaseI/task4): nvs_flash direct (item CC)`
- Pushed to `origin/feat/phaseI-task4-nvs-flash`. No PR opened (per instructions).

## Changes Implemented

### CC — EepromManager migrated off Arduino Preferences
- `firmware/main/src/persistence/EepromManager.h`: `#include <Preferences.h>` →
  `#include <nvs.h>` + `#include <nvs_flash.h>`. `detail::State` drops its
  `Preferences prefs` member — now a plain POD (`isInitialized`, `isDevMode`,
  `devEpoch`), since each accessor opens/commits/closes its own short-lived
  `nvs_handle_t` rather than holding one handle open for the process
  lifetime. All public function names + signatures in `lattice::eeprom::*`
  are unchanged.
- `firmware/main/src/persistence/EepromManager.cpp`: every `_state.prefs.*`
  call point replaced with a small set of private `nvs*` helpers
  (`nvsGetU8/nvsPutU8`, `nvsGetU32/nvsPutU32`, `nvsGetBool/nvsPutBool`,
  `nvsGetBytes/nvsPutBytes`, `nvsRemove`, `nvsHasKey`, `nvsClearAll`) that
  each open `NVS_KEYS::NAMESPACE` ("lattice") read/write, do one
  get/set/erase, commit on writes, and close. `persistOrEscalate()` (the
  Phase A tiered write-failure escalation wrapper) is untouched — every
  helper still surfaces "bytes written" (0 on any failure) so its
  `got == want` check works exactly as before; security-relevant keys
  (mesh key, keypair, known-master MACs, peer list, boot epoch) still
  escalate via `lattice::err::fail` on a short/failed write, non-security
  keys still just log at ERROR. `Cleanup` (the static-teardown
  `_state.prefs.end()` destructor) is removed — nothing to close anymore
  with the per-operation handle pattern.
- Namespace string (`"lattice"`) and every key name (`NVS_KEYS::*`) are
  byte-for-byte unchanged, so **no on-flash layout change and no reflash
  required** for this task alone.

### main.cpp — nvs_flash_init() at boot
- Added `#include <nvs_flash.h>`.
- Added, as the first statements in `setup()`, before `Serial.begin()` and
  before the first `lattice::eeprom::init()` call:
  ```cpp
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);
  ```
  marked with comment `// Phase I Task 4: nvs_flash direct`, matching plan
  Step 2 exactly.

### Host test mock — Option (a): mock the real nvs_flash API
- Added `tests/mocks/nvs.h`, `tests/mocks/nvs_flash.h`,
  `tests/mocks/nvs_flash_mock.cpp` — a from-scratch mock of the handle-based
  ESP-IDF `nvs_open/nvs_get_*/nvs_set_*/nvs_commit/nvs_close/nvs_erase_*`
  API (function signatures cross-checked line-for-line against
  `$IDF_PATH/components/nvs_flash/include/nvs.h`, v5.5.1 — all match
  exactly). Backed by an in-memory `map<"namespace/key", vector<uint8_t>>`,
  the same keying scheme the old `Preferences::_store` mock used.
  Deliberately mirrors two real-IDF semantics this firmware's call patterns
  actually depend on:
  - `nvs_get_blob(handle, key, NULL, &length)` — size-query mode, used by
    `hasPeers()`'s replacement (`nvsHasKey`) since NVS has no
    type-independent "does this key exist" accessor and `PEER_LIST` is only
    ever stored as a blob.
  - A too-small output buffer returns `ESP_ERR_NVS_INVALID_LENGTH` with
    *no* partial copy (real IDF behavior) rather than the old Preferences
    mock's silent `std::min()`-truncate — not exercised by any current
    caller (every fixed-size load sizes its buffer to match), but avoids
    silently masking a future bug.
  - `NvsMock::_store` / `NvsMock::_failNextWriteKey` are the direct
    replacements for `Preferences::_store` / `Preferences::_shortWriteKey`,
    used by `tests/unit/test_eeprom_manager.cpp`'s short-write/corruption
    injection tests and `tests/e2e/harness/NodeContext.cpp`'s per-node
    snapshot/restore.
- Deleted `tests/mocks/Preferences.{h,cpp}` — confirmed nothing else in the
  tree referenced them (`grep -rl Preferences firmware/ tests/` before the
  change showed only EepromManager + its two test/harness consumers).
- `tests/CMakeLists.txt`: swapped `mocks/Preferences.cpp` →
  `mocks/nvs_flash_mock.cpp` in `FIRMWARE_SOURCES`.
- `tests/unit/test_eeprom_manager.cpp`,
  `tests/e2e/harness/NodeContext.{h,cpp}`: `Preferences::_store` →
  `NvsMock::_store`, `Preferences::_shortWriteKey` →
  `NvsMock::_failNextWriteKey`, `#include <Preferences.h>` →
  `#include <nvs.h>`, `NodeContext::prefsStore` field renamed
  `nvsStore`.

### UU — CRC16 swap: **skipped**
Checked the real, locally-installed ESP-IDF header
(`$HOME/esp/esp-idf/components/esp_rom/include/esp_rom_crc.h`, v5.5.1)
rather than taking the brief's premise on faith. Its own doc comment gives
the variant table:
```
CRC-16/CCITT,       poly=0x1021, init=0x0000, refin=true,  refout=true   -> crc16_le
CRC-16/CCITT-FALSE, poly=0x1021, init=0xffff, refin=false, refout=false  -> crc16_be (with pre/post XOR)
```
`esp_rom_crc16_le` is the **reflected** CRC-16/CCITT variant (refin=true,
refout=true) — not CRC-16/CCITT-FALSE. `EepromManager.cpp`'s hand-rolled
loop (`crc16()`, MSB-first, init 0xFFFF, no reflect, no xor-out) is
CRC-16/CCITT-FALSE, which per the same header's usage note actually needs
`esp_rom_crc16_be` plus a pre/post-inversion transform (`~crc16_be(~init,
...)`), not `crc16_le`. Since the brief's exact-match condition doesn't
hold, and this CRC only ever compares against itself (write vs. read, no
external interop, so correctness doesn't strictly require a specific
algorithm) — swapping to a reflected variant with an unverified transform
for zero measured benefit isn't worth the risk to a security-relevant
integrity check (keypair CRC) without dedicated test vectors. Left the
hand-rolled loop in place with a comment in `EepromManager.cpp` explaining
the finding and pointing at the real header.

## Tests

### Host (293/293 pass)
```
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && cmake -B build . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --parallel 1
```
Result: `100% tests passed, 0 tests failed out of 293`. All 53
`EEPROMMgrTest.*` cases pass unchanged (short-write escalation,
CRC-corruption, dev-mode RAM-only boot epoch, peer-list 0xFF-prefill, etc.)
— confirms the nvs mock preserves every observable behavior the old
Preferences mock had.

### ESP-IDF build (clean)
```
export IDF_PATH=$HOME/esp/esp-idf
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.9_env
XTENSA=$(ls -d $HOME/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
export PATH=$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$XTENSA:$PATH
cp -f firmware/main/config/master_pubkey_pin.h.example firmware/main/config/master_pubkey_pin.h
cd firmware && python $IDF_PATH/tools/idf.py reconfigure
python $IDF_PATH/tools/idf.py build -- -j2
```
(Note: `idf.py build` doesn't take a bare `-j2`; parallelism is passed to
the underlying Make invocation via `idf.py build -- -j2`.)

Result: clean build, no warnings or errors in any Task-4-touched file
(`EepromManager.{h,cpp}`, `main.cpp`). `git submodule update --init
--recursive` was required first — `firmware/main/lib/lattice-protocol` was
uninitialized in this worktree (unrelated to this task; the build fails
with a `mesh_message.h` not-found error without it).

## Size Delta (idf.py size, pre- vs. post-Task-4)

Baseline captured by `git stash` back to `a6f8070` (this branch's base) and
rebuilding, then `git stash pop` to restore Task 4's changes and rebuild
again — both measured with the same toolchain/sdkconfig in this worktree.

| Section  | Baseline (a6f8070) | Task 4    | Delta      |
|----------|--------------------:|----------:|-----------:|
| .text    |              638004 |    636340 |     -1664  |
| .rodata  |              134900 |    134192 |      -708  |
| .bss     |               21792 |     21784 |        -8  |
| .data    |               15984 |     15936 |       -48  |
| **Total image** |        **882931** | **880511** |  **-2420** |

Binary shrinks by ~2.4 KB — dropping the Preferences C++ wrapper layer
(and its `String`-based namespace-key bookkeeping) in favor of calling
`nvs_open`/`nvs_get_*`/`nvs_set_*` directly costs less code than routing
through it.

## Concerns / Follow-ups
- The per-operation `nvs_open`/`nvs_close` pattern (one open+close per
  accessor call, per the plan's Step 3 example) trades a small amount of
  runtime overhead (repeated handle open/close) for simpler, more
  ESP-IDF-idiomatic code and no held-open write handle across the process
  lifetime. Not measured for latency impact — NVS operations are already
  infrequent (boot-time config load, occasional peer/keypair/MAC writes),
  so this is expected to be a non-issue, but flagging it as a design choice
  worth knowing about if a future task needs tighter NVS-path latency.
  Preserves the same reasoning the Task-4 brief itself uses (Step 3
  example), so no deviation from plan intent.
- `firmware/main/lib/lattice-protocol` submodule was uninitialized in this
  worktree; had to run `git submodule update --init --recursive` before
  either the host or ESP-IDF build would succeed. Unrelated to this task's
  changes but worth flagging in case other parallel task worktrees hit the
  same thing.
- UU (CRC16 swap) skipped — see rationale above. No code change beyond an
  explanatory comment at the `crc16()` definition.
