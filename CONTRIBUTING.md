# Contributing to Planetopia Firmware

Planetopia follows Tiger-Style engineering principles: safety first, performance always, zero technical debt.  The CI pipeline enforces these rules automatically.  Every pull-request **must** pass all gates before merge.

## 0. Quick checklist before opening a PR

- [ ] `arduino-cli compile -e --fqbn esp32:esp32:esp32da main` builds with **no warnings** (run locally — not in CI).
- [ ] `clang-format -style=file` applied; `git diff --check` shows no whitespace errors.
- [ ] No `new`, `malloc`, or unbounded `std::vector` growth after setup.
- [ ] All errors funnel through `src/error/Error.h` (`planetopia::err::*`).
- [ ] MAC handling uses `planetopia::utils::MacAddress` helper.
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
if (!planetopia::err::checkEsp(esp_now_init(), utils::ErrorType::COMMUNICATION_FAIL,
                               "esp_now_init failed"))
    return false;
```
Assertions live in unit-tests, not production.

## 5. Memory policy
* All dynamic containers reserved at start-up.
* No heap allocation (`new`, `malloc`) after `setup()`.

## 6. CI pipeline (GitHub Actions)

The workflow runs automatically on every push and PR:

- **unit-tests** — CMake build + CTest (Linux native, no ESP32 toolchain needed)
- **lint-format** — `clang-format --dry-run --Werror` over all `main/src/*.{h,cpp}`
- **static-analysis** — `cppcheck` with `--error-exitcode=1`

PR merges are blocked until all three jobs are green.

> **Note:** Arduino / ESP32 toolchain compilation is not in CI (large binary
> download, ~10 min). Run `arduino-cli compile --fqbn esp32:esp32:esp32da main`
> locally before submitting a PR that touches firmware source.

---
Thank you for keeping Planetopia rock-solid! 💪
