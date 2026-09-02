# Contributing to Lattice Firmware

Lattice follows Tiger-Style engineering principles: safety first, performance always, zero technical debt.  The CI pipeline enforces these rules automatically.  Every pull-request **must** pass all gates before merge.

## 0. Quick checklist before opening a PR

- [ ] `cd firmware && idf.py build` (ESP-IDF v5.5.1) builds with **no warnings** — CI's
      **Firmware Build** job compiles it too, but check locally first.
- [ ] `clang-format -style=file` applied; `git diff --check` shows no whitespace errors.
- [ ] No `new`, `malloc`, or unbounded `std::vector` growth after setup.
- [ ] All errors funnel through `src/error/Error.h` (`lattice::err::*`).
- [ ] MAC handling uses `lattice::utils::MacAddress` helper.
- [ ] Added/updated unit tests (if applicable).
- [ ] Updated documentation (README / docs/) if behaviour changes.

## 1. Branch naming
```
feature/<topic>    # new features
fix/<bug>          # bug fixes
refactor/<area>    # structural changes
```

## 2. Commit style
* Use imperative present-tense: “Add error façade”, “Fix MAC formatting”.
* Limit to 72-char first line; include body if needed.

## 3. Code style
* `clang-format` (LLVM style with project overrides) is canonical.
* No anonymous `using namespace` in headers.
* Prefer fixed-width types (`uint8_t`, `int32_t`) over `int`.
* Keep functions < 70 lines; break out helpers otherwise.

## 4. Error handling
```cpp
if (!lattice::err::checkEsp(esp_now_init(), utils::ErrorType::COMMUNICATION_FAIL,
                               "esp_now_init failed"))
    return false;
```
Assertions live in unit-tests, not production.

## 5. Memory policy
* All dynamic containers reserved at start-up.
* No heap allocation (`new`, `malloc`) after `setup()`.

## 6. CI pipeline (GitHub Actions)

The workflows run automatically on every push to `main`/`develop` and every PR:

- **unit-tests** — CMake build + CTest (Linux native, no ESP32 toolchain needed)
- **e2e** — the host-side multi-node mesh simulation suite (ctest label `e2e`)
- **firmware-build** — ESP-IDF v5.5.1 `idf.py build` + size report (stubs `master_pubkey_pin.h`
  from the committed `.example`)
- **lint-format** — `clang-format --dry-run --Werror` over all `main/src/*.{h,cpp}`
- **static-analysis** — `cppcheck` with `--error-exitcode=1`
- **proto-sync** — regenerates `mesh.pb.h`/`.c` from the `lattice-protocol` submodule and fails on
  drift (`tools/gen_mesh_pb.sh --check`)

PR merges are blocked until every job is green. (CodeQL and Dependency Review run alongside
these as security gates.)

> **Note:** the firmware is built with ESP-IDF only — there is no Arduino IDE / `arduino-cli`
> path. See the README's Requirements / Quick Start for toolchain setup.

---
Thank you for keeping Lattice rock-solid! 💪
