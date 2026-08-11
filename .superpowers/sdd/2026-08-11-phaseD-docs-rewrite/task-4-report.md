# Task 4 Report — `docs/error_codes.md` rewrite

**Status:** DONE

**Commit:** `439dc17` on branch `phaseD-task4-manual` ("docs(phaseD): rewrite error_codes.md with the current digit-based registry")

## What was done

Full rewrite of `docs/error_codes.md`, replacing the stale doc (whose own worked example called
the deleted legacy `fail(utils::ErrorType, const char*)` overload) with an accurate description of
the current digit-based error-code system, per the brief's outline:

1. **How codes work** — `TMS` 3-digit decimal, `makeErrorCode(t,m,sub) = t*100 + m*10 + (sub%10)`,
   the current `ErrorTypeDigit` (GENERIC=1..CRYPTO=7) and `ModuleDigit` (CORE=1..HW=5) enums, and
   an explicit call-out of the `sub % 10` behavior (two real call sites, `E2ECrypto.h:21`/`:44`,
   pass sub-code literals `25`/`26` that display as `5`/`6`).
2. **Current public API** — `lattice::err::fail(ErrorTypeDigit, ModuleDigit, uint8_t, const char*)`
   / `fatal(...)`, the `check()`/`checkEsp()` wrappers (always `ModuleDigit::CORE`, sub `0`), and an
   explicit statement that the old 2-arg `fail(utils::ErrorType, const char*)` overload is fully
   removed and will not compile — replacing the old doc's example that called it.
3. **Registry table** — transcribed all 35 rows from the research verbatim: 28
   `firmware/main/src` call sites in one table, plus the 7 `main.cpp` call sites in a second table
   (mirroring the research's own two-table split), each with file:line, call kind (`fail`/`fatal`),
   T/M/S, resulting code, exact message string, and trigger. No row was summarized, merged, or
   dropped.
4. **Known code collisions** — a dedicated section listing all 4 collision pairs from the research
   (621: PirAdapter.cpp:29 / Adapter.cpp:37; 622: PirAdapter.cpp:36 / AdapterFactory.cpp:34; 651:
   SevenSegDisplay.cpp:44 / Led.cpp:32; 552: SevenSegDisplay.cpp:127 / Led.cpp:48), plus the
   research's point that the disambiguating log message is compiled out by default
   (`LATTICE_DEFAULT_LOG_LEVEL` = `LATTICE_LOG_LEVEL_NONE`), so the numeric code alone cannot
   distinguish them on a production build.
5. **TM1637 display mapping** — `signalError()` → `makeErrorCode()` → `SevenSegDisplay::show()`,
   leading-zero padding, leftmost-digit-blank-then-T-M-S rendering; separately, the coarser
   LED-blink-count mapping (`blinkPattern()`: HARDWARE=6, COMM=3, MEMORY=4, CONFIG=5, CRYPTO=6,
   default=1) and the `MEMORY`/`HARDWARE` → `esp_restart()` behavior.
6. **Adding a new code** — updated worked example using the real digit-based API, pointing at the
   actual `PeerRegistry.cpp:154` call site (code 432) as a concrete real example instead of an
   invented one, plus a checklist (pick T/M already matching the call site, avoid new collisions
   given the `sub % 10` fold, choose `fail` vs `fatal`, and update the registry table).

## Verification performed

- Read the full research file
  (`/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-persistence-crypto-error.md`)
  end to end before writing anything.
- Read `firmware/main/src/error/ErrorCodes.h` directly and confirmed the `ErrorTypeDigit`/
  `ModuleDigit` enum values and the `makeErrorCode` formula (including the `sub % 10`) match the
  research and the doc exactly, word-for-word on the formula.
- Read `firmware/main/src/error/Error.h`'s `fail()`/`fatal()` signatures directly, confirming the
  four-argument form and that no 2-arg overload exists in the header.
- **Self-check (required by the brief): spot-checked 8 rows** (more than the requested 5) against
  the actual source, opening each file and confirming the `err::fail`/`err::fatal` call's T/M/S
  arguments and line number:
  - `mesh/Mesh.cpp:43` — HARDWARE,MESH,1 confirmed.
  - `mesh/E2ECrypto.h:21` and `:44` — CONFIG,MESH,25 and CONFIG,MESH,26 confirmed at those exact
    line numbers (`grep -n` cross-check).
  - `adapter/pir/PirAdapter.cpp:29` — CONFIG,ADAPTER,1 confirmed.
  - `persistence/eeprom/EepromCore.cpp:51` and `:208` — MEMORY,EEPROM,5 and MEMORY,EEPROM,1
    confirmed.
  - `main.cpp:387` and `:459` — MEMORY,CORE,2 and COMM,MESH,1 confirmed.
  - `mesh/MasterBeacon.cpp:120` — CONFIG,MESH,7 confirmed (line 120 is the `fail(` call itself).
  - `hardware/output/Led.cpp` — all 5 calls (lines 32/48/60/72/115) confirmed via one grep against
    the T/M/sub triples used in the table.
  - `adapter/serial/SerialAdapter.cpp:254` — CONFIG,ADAPTER,6 confirmed.
  - All matched the research and the transcribed table exactly; zero discrepancies found.
- No code was modified — `git status` shows only `docs/error_codes.md` changed plus this report.

## Self-assessment

**Full transcription, not a summary.** All 35 registry rows (28 + 7) were transcribed from the
research with every field (file:line, call kind, T/M/S, code, message, trigger) preserved exactly
as given, split into the same two tables the research uses. The four documented collisions, the
two `sub % 10` special cases, and the four `check()`/`checkEsp()` wrapper call sites were also
carried over in full — nothing from the research's registry was omitted or rounded. Numeric codes
were not retyped by hand from the T/M/S arithmetic; they were copied as the research already
computed them, then spot-checked against source for 8 rows spanning every file area (mesh, adapter,
hardware, eeprom, main.cpp) with no mismatches.

## Concerns

None blocking. One judgment call worth flagging for the controller: the brief's outline lists the
registry as a single "35-row table," but the research itself presents it as two tables (28-row
`firmware/main/src` table + 7-row `main.cpp` table, the latter without a leading `#` column). I
kept that same two-table split in the doc rather than force-merging into one continuous
numbered table, since renumbering the `main.cpp` rows 29-35 would have meant inventing numbering
the research doesn't provide. All 35 rows are present and fully transcribed either way.
