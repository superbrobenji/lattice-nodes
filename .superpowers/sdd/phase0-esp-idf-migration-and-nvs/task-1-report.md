# Task 1 Report: Restructure Directory Layout for ESP-IDF

## Status
**DONE**

## Commits
- `06f4df0` — feat: restructure directory layout for ESP-IDF migration (Task 1)
- `35042d3` — fix: move lattice-protocol submodule to firmware/main/lib/

## Changes Implemented

### Directory Restructure
- Moved `main/` to `firmware/main/` using git mv (68 files)
- Renamed `main.ino` → `main.cpp` with forward declarations added
- Moved lattice-protocol submodule: `main/lib/lattice-protocol` → `firmware/main/lib/lattice-protocol`

### ESP-IDF Skeleton Files Created
1. **firmware/CMakeLists.txt** — Top-level project file (ESP-IDF 5.0+)
2. **firmware/main/CMakeLists.txt** — Component file listing all sources and includes
   - All source directories under `src/` included
   - No non-existent directories listed (crypto headers are in `src/mesh/`)
   - REQUIRES: `esp_wifi` (provides esp_now), `nvs_flash`, `driver`, `mbedtls`
3. **firmware/main/idf_component.yml** — Component manifest (IDF ≥5.0.0)
4. **firmware/sdkconfig.defaults** — Default configuration (FreeRTOS, partition table, crypto, NVS, watchdog)
5. **firmware/partitions.csv** — Partition table (nvs, phy_init, factory)

### Path Updates
- **tests/CMakeLists.txt**: All `../main/` → `../firmware/main/` (2 blocks)
- **.github/workflows/unit-tests.yml**: All `main/src` → `firmware/main/src` (2 blocks: clang-format, cppcheck)
- **.gitmodules**: Updated submodule section name and path

### Forward Declarations
Added to `firmware/main.cpp` after includes:
```cpp
// Forward declarations (Arduino .ino auto-generates these)
void setup();
void loop();
```

## Tests
**All tests passed: 184/184**

Build command:
```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && \
cmake --build tests/build --parallel && \
ctest --test-dir tests/build --output-on-failure --parallel 4
```

Results:
- Unit tests: 145/145 ✓
- E2E tests: 39/39 ✓
- Total runtime: 0.17s

## Verification Checklist
- ✓ Git history clean (used `git mv` for renames)
- ✓ Submodule path correct and initialized
- ✓ All source directories enumerated in IDF CMakeLists.txt
- ✓ No non-existent directories (e.g., `src/crypto`) listed
- ✓ Host test suite green (100% pass rate)
- ✓ CI workflow paths updated for linting and static analysis
- ✓ Forward declarations added to main.cpp
- ✓ mesh.pb.h/mesh.pb.c untouched (hand-edited files)

## Open Questions / Concerns
None — task completed successfully. All tests pass, directory structure matches ESP-IDF conventions, and CI paths are updated.

## Next Steps
Ready for Task 2: NVS API wrapper implementation.
