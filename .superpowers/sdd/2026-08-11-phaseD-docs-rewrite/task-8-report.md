# Task 8 Report — `docs/getting_started.md` (new)

**Status:** DONE

**Commit:** `aee6258` on branch `phaseD-task8-manual`
("docs(phaseD): add getting_started.md, a thorough non-technical build-from-source guide")

## What was done

Created `docs/getting_started.md` (685 lines) — a new, extremely thorough, zero-prior-experience
build/flash/configure/operate guide for the firmware, per the brief's 13-section outline:

1. **What you'll end up with** — plain-language framing of sensor-node vs. master-node roles, and
   the "you'll also need `lattice-hub` for a complete operating system" callout (using the plan
   header's ecosystem-overview paragraph as the source for that framing), explicitly scoping the
   hub setup itself as out of scope.
2. **Hardware you'll need** — points to `docs/hardware_requirements.md` (Task 7's doc — confirmed
   it does not yet exist in this worktree, as expected; linked anyway per the brief since both land
   in the same PR).
3. **Step 1: Install ESP-IDF** — full per-OS instructions (macOS/Linux via `install.sh`/`export.sh`,
   Windows via the graphical installer or `install.bat`/`export.bat`), pinned to **v5.5.1** per
   `firmware/dependencies.lock`, plus Linux apt prerequisites and an `idf.py --version` verification
   step with the exact expected output line.
4. **Step 2: Clone + submodule init** — `git clone`, `git submodule update --init --recursive`,
   what a submodule is in one sentence, and the verification command
   (`ls firmware/main/lib/lattice-protocol`) with "populated vs. empty" as the success signal.
5. **Step 3: Generate `master_pubkey_pin.h`** — the critical, previously-undocumented prerequisite.
   Walked through the exact throwaway-key command sequence from the research (verbatim), explained
   the `#error` gate's purpose as a security feature, explicitly noted the file/`masterkey.json` are
   gitignored/per-deployment and must never be committed, and explained the real-deployment path
   (regenerate against the hub's real `masterkey.json` + real MAC before shipping).
6. **Step 4: Configure `project_config.h`** — full constant-by-constant reference: a "change this
   now" table (just `DEFAULT_MESH_KEY`, with an exact key-generation command and an explicit
   warning about the Python-list-vs-C-array bracket mismatch beginners would hit), a "leave at
   defaults" reference table covering every Section 1–8 constant from the research, and a separate
   "advanced: mesh-scale limits" table covering all 11 Section 9 (Tiger Style) constants — the
   research explicitly recommended a one-line-each mention for these even though they're not
   first-build-critical.
7. **Step 5: Build** — `idf.py set-target esp32` / `idf.py build`, the verbatim
   "Project build complete" success output from the research's real build log, and a "common
   failures" callout pointing back to Steps 2 and 3 specifically (missing submodule vs. missing pin
   file, with the actual compiler error text for each so a reader can pattern-match).
8. **Step 6: Flash** — per-OS serial port discovery (macOS `ls /dev/tty.*` before/after, Linux
   `/dev/ttyUSB0`, Windows Device Manager), `idf.py -p PORT flash`, and expected output including
   the "Hard resetting via RTS pin..." success line the brief specifically calls out.
9. **Step 7: First boot — provisioning** — `idf.py -p PORT monitor`, the exact `LATTICE_PUBKEY:`
   format (64 uppercase hex chars) from the research, `Ctrl+]` to exit, and a pointer to
   `docs/server_requirements.md`'s "Enrollment Protocol" section for the server-side technical
   detail rather than duplicating it.
10. **Step 8: LEDs and display** — the full blink-pattern table (startup, mesh-activity, role-toggle
    2-vs-3, reset-arm, reset-confirm, the 1–8 error-code-by-blink-count table, and the fatal
    green-fallback pattern) and the seven-segment display states (dashes/number/number+decimal),
    all transcribed from the research and written in plain "if you see X, it means Y" language.
    Flagged the known blink-count-vs-numeric-code inconsistency (see Concerns below) rather than
    silently reconciling it.
11. **Step 9: Buttons** — config button (5s hold, role toggle, production auto-restart) and reset
    button (5s arm + 5s confirm within a 3s window, full EEPROM wipe), with exact timings and a
    "count out loud" framing for non-technical readers, plus the practical consequences of each
    action spelled out.
12. **Troubleshooting** — the 3 FAQ entries the brief required (`master_pubkey_pin.h` build error,
    can't find serial port, node won't enroll) plus 3 more for extra thoroughness (missing-submodule
    build error, `idf.py` not found, flash succeeds but nothing prints). The driver note
    (CP210x/CH340) is explicitly flagged as general USB-serial-chip knowledge, not something this
    repo documents or ships, since a repo-wide check found no repo-specific driver docs to cite.
13. **What's next** — links to `README.md`'s Development section (for the test suite) and
    [GitHub issue #101](https://github.com/superbrobenji/lattice-nodes/issues/101) (confirmed live
    via `gh issue view 101`, title matches the brief exactly) for the future pre-built-binary path.

## The `master_pubkey_pin.h` step — confirmation

This is present as its own dedicated numbered step (**Step 3**, section 5 of the doc), positioned
exactly where the brief specifies: after the submodule step, before the first build. It:
- States up front, in bold, "Do not skip this step" and that skipping it fails the very first build.
- Quotes the real `#error` message verbatim (matches `master_pubkey_pin_wrapper.h` exactly, verified
  by reading the actual file in this worktree, not just the research).
- Gives the exact two-command throwaway-key sequence, matches the research's real, tested command
  sequence character-for-character (`python3 -c "import json,base64,os; ..."` then
  `python3 tools/gen_master_pubkey_pin.py masterkey.json aa:bb:cc:dd:ee:ff`).
- Explains what "success" looks like (`wrote /path/.../master_pubkey_pin.h`) and how to verify via
  `ls firmware/main/config/`.
- Explains the real-deployment path (regenerate against the hub's actual `masterkey.json` + MAC)
  and the gitignore/never-commit rule for both `masterkey.json` and the generated header.
- Is cross-referenced from both the Step 5 (Build) failure callout and the Troubleshooting FAQ's
  first entry, so a reader who hits the compile error from *either* direction finds their way back
  to this step.

## Verification performed

- Read Part 2 of the research file in full
  (`/private/tmp/claude-501/.../scratchpad/phaseD-research-build-flash-config.md`, lines 722–931)
  for the exact command sequence, `project_config.h` reference, provisioning flow, and LED/display
  tables; also read Part 1 (build blocker + real build/measurement output) for the verbatim
  `#error` text and successful-build output.
- Cross-checked every fact against live source in this worktree rather than trusting the research
  blindly:
  - `firmware/main/config/master_pubkey_pin_wrapper.h` — read directly, `#error` text matches.
  - `tools/gen_master_pubkey_pin.py` — read directly, confirmed exact CLI usage/arg order/output
    format used in the doc.
  - `firmware/main/project_config.h` — read directly; every constant, comment, and default value in
    the doc's reference tables matches the live file (not just the research's transcription of it).
  - `.gitmodules`, `.gitignore` — confirmed submodule path/URL and the `master_pubkey_pin.h`
    gitignore entry.
  - `firmware/partitions.csv` — confirmed the 1 MiB `factory` app partition referenced in the
    Step 5 "what success looks like" explanation.
  - `firmware/main/src/error/ErrorCore.cpp` — read directly to confirm the 1–8 blink-count-to-
    `ErrorType` mapping used in the LED table (this also surfaced the inconsistency noted below).
  - `docs/server_requirements.md` — confirmed the "Enrollment Protocol (node provisioning)" heading
    exists (line 253) before linking to it.
  - `docs/superpowers/plans/2026-08-11-phaseD-docs-rewrite.md` — read the plan header's inline
    "Ecosystem overview" paragraph (no separate file exists for it) and used it verbatim as the
    source for the "you'll also need a server" framing in section 1.
  - `gh issue view 101` — confirmed issue #101 exists, is open, and its title matches what the brief
    and this doc claim.
- Verified all 27 internal anchor links resolve to real headings by writing a small script that
  reproduces GitHub's heading-slug algorithm (lowercase, strip punctuation including em dashes,
  spaces→hyphens) and diffing every `#anchor` link against every heading's computed slug — all
  resolved, including the tricky em-dash case in "Step 7: First boot — provisioning" (which slugs to
  a double-hyphen, `...first-boot--provisioning`, because GitHub strips the em dash rather than
  converting it to a hyphen).
- Checked markdown-table well-formedness (`awk`) and code-fence balance (`grep -c` on triple-
  backtick lines, confirmed even count).
- Confirmed `docs/hardware_requirements.md` does not yet exist in this worktree (expected — Task 7
  runs in a sibling worktree) but linked it anyway per the brief.
- Caught and corrected two of my own drafting mistakes before finalizing: a garbled sentence in the
  reset-button "practical consequence" paragraph (leftover from an edit), and a wrong byte count in
  the illustrative flash-output example (had used the research's "total ELF image size" figure,
  664131 bytes, in a spot meant to illustrate the flashed *app binary* size — corrected to the
  actual `0xa22b0`-byte app binary size, 664240 decimal, computed and verified via `python3`).

## Self-assessment

Every command in the doc is either transcribed verbatim from the research's real, tested command
output (Part 1's actual build) or cross-verified directly against live source in this worktree
(`project_config.h`, the pin-generator script, the wrapper header, `.gitmodules`, `.gitignore`,
`partitions.csv`). No numeric constant, pin number, file path, or command was invented or taken
from memory. The one place I deliberately used illustrative (not literally-captured) output is the
flash-progress example in Step 6, since the research never flashed a physical device — this is
flagged in my own read-through and the numbers used there are drawn from real, verified figures
elsewhere in the research (the app binary size) rather than fabricated, with the surrounding prose
using "e.g." to signal it's representative rather than a literal transcript.

## Concerns

None blocking. Two things worth flagging for the controller/reviewer:

- **Known code inconsistency, documented not fixed** (per the plan's Global Constraints): the
  seven-segment display's numeric error code (`makeErrorCode(T, M, sub)`, documented in
  `docs/error_codes.md`) and the red-LED blink-count-by-`ErrorType` scheme (1–8, driven by
  `ErrorCore.cpp`'s `blinkPattern()`) are two separately-numbered systems that happen to share some
  digit values but aren't guaranteed to agree on a given fault. I called this out explicitly in the
  Step 8 display table rather than presenting them as one unified scheme, and advised trusting the
  more specific numeric code if the two ever seem to disagree — this matches the plan's instruction
  to document known gaps rather than silently smooth them over or attempt a code fix (out of scope,
  docs-only phase).
- `README.md` in this worktree is still the pre-Task-2 stale version (Arduino/`main.ino`/
  `arduino-cli`-era content) since Task 2 runs in a sibling worktree and hasn't merged yet. I linked
  to its "Development" section by name (as instructed) rather than duplicating test-suite
  instructions here, trusting Task 2 to land that section correctly in the same PR — I did not
  verify the link text/anchor against the *current* README's structure since it will be rewritten
  before merge; the link target (`README.md`, no anchor used) is stable regardless of Task 2's
  internal restructuring.
