# Task 2 Report — `README.md` rewrite

**Status:** DONE

**Commit:** `6296de8` on branch `phaseD-task2-manual` ("docs(phaseD): rewrite README.md for the post-A/B/C/E architecture")

## Note on brief location

The controller pointed at `.superpowers/sdd/2026-08-11-phaseD-docs-rewrite/task-2-brief.md`, which
did exist in this worktree (unlike Task 1's experience). I additionally read the full plan file at
`docs/superpowers/plans/2026-08-11-phaseD-docs-rewrite.md` for the "ecosystem overview" the brief
references as "this plan's header section" (not reproduced in the standalone brief file) — that
section (lines 27, 73, 90 of the plan) supplied the `lattice-hub`/`lattice-protocol` ecosystem
facts used in the new README's Ecosystem section.

## What was done

Full rewrite of `README.md` per the brief's 13-section outline: Overview, Ecosystem, Features,
Architecture (short tree + link to `REFACTORING_GUIDE.md`), Requirements, Quick Start,
`project_config.h` Reference, Buttons, Seven-Segment Error Codes, Server Integration, Development,
Adding a New Adapter, Contributing/License/Security. Old stale content removed: `main.ino`,
Arduino IDE/`arduino-cli` instructions, the capitalized `main/src/Mesh/` path, `EEPROM_Manager.h`,
`MacAddress.h`, the 75-byte/12-byte-data message struct table, and the old dataType numbering.

## Verification performed (source-grounded, not transcribed from memory)

- **`project_config.h` reference table**: every constant name, default value, and pin number
  cross-checked directly against `firmware/main/project_config.h` via `grep -n` in this session
  (not just the research file) — `DEV_MODE=false`, `DEFAULT_DEV_MASTER=true`,
  `MASTER_BEACON_INTERVAL_MS=3000`, `STALE_MASTER_THRESHOLD_MS=9000`, `DUAL_MASTER_MODE=false`,
  `WIFI_CHANNEL=1`, `RED_LED_PIN=33`, `GREEN_LED_PIN=26`, `CONFIG_BUTTON_PIN=32`,
  `RESET_BUTTON_PIN=25`, `SEVSEG_DATA_PIN=23`, `SEVSEG_CLK_PIN=22`, `ENABLE_SEVSEG_DISPLAY=true`,
  `DEFAULT_TX_POWER_PRESET=TxPowerPreset::OUTDOOR`, `SIMULATE_MODE=0`, `MAX_HOPS=8`,
  `HEALTH_REPORT_INTERVAL_MS=30000` all confirmed live in source, all match the research file too.
- **Error code digit enums**: read `firmware/main/src/error/ErrorCodes.h` directly and found the
  old README's table was incomplete (missing `CRYPTO=7` in `ErrorTypeDigit`) and wrong about the
  sub-code range (old doc said "0–F" i.e. hex; the actual formula is `t*100 + m*10 + (sub % 10)`,
  a decimal `TMS` code, so sub-code is 0–9, not hex). Fixed both in the new table.
- **Low-power CPU scaling claim**: the old README's "80 MHz / master 240 MHz" claim wasn't directly
  covered by the build-flash-config research file, so I independently verified it by reading
  `firmware/main/main.cpp:564-569` — confirmed `esp_pm_config_t{max_freq_mhz: isMaster?240:80,
  min_freq_mhz: 80, light_sleep_enable: true}`. Corrected the README's wording to be precise: the
  master runs a *dynamic* 80–240MHz DFS range (not fixed 240MHz), leaves are *pinned* at 80MHz,
  and light sleep is enabled for both.
- **Requirements section — deliberate deviation from the brief's literal wording**: the brief says
  to source the target chip from "`sdkconfig.defaults`'s `CONFIG_IDF_TARGET`", but I confirmed via
  `grep` that `firmware/sdkconfig.defaults` (the only tracked sdkconfig file — `firmware/sdkconfig`
  itself is gitignored/generated) does **not** contain `CONFIG_IDF_TARGET` at all; that constant
  only appears in the generated, gitignored `sdkconfig` after running `idf.py set-target esp32`.
  I worded the Requirements table to state the target is set via `idf.py set-target esp32` rather
  than falsely claiming it's pinned in `sdkconfig.defaults`. Similarly worded the ESP-IDF version
  line to note `firmware/dependencies.lock` is auto-generated (also gitignored, not present until
  first build) rather than implying it's a committed file a reader can inspect pre-build.
- **Message struct**: per the brief's "must get right," stated 200 bytes / protocol v5 / 64-byte
  data field in one sentence (matching `phaseD-research-wire-protocol.md` §1, itself sourced from
  `firmware/main/lib/lattice-protocol/c/mesh_message.h`'s `static_assert(sizeof(mesh_message) ==
  200)`) and linked to `docs/server_requirements.md` rather than reproducing the 15-row field
  table, per the brief's explicit guidance to avoid duplication.
- **Development section commands**: cross-checked against `.github/workflows/unit-tests.yml` and
  `e2e-tests.yml` directly (not just the old README) — added the `--parallel 2` cap and the
  `--target lattice_e2e` build step to match what CI actually runs (the old README's unit-test
  command had no explicit parallelism cap; CI documents an explicit reason for capping at 2 to
  avoid OOM on its 2-core runner, so I matched that rather than leaving it unbounded).
- **Link resolution self-check**: confirmed every linked file exists via `ls`/`find` —
  `REFACTORING_GUIDE.md`, `docs/error_codes.md`, `docs/server_requirements.md`,
  `docs/adapter_development_guide.md`, `docs/design-gaps/multihop-data-uplink.md`,
  `CONTRIBUTING.md`, `LICENSE`, `SECURITY.md` all present. `docs/getting_started.md` does **not**
  yet exist in this worktree (Task 8 is a sibling in-progress worktree creating it) — this is
  expected and acceptable per the brief's explicit coordination note ("if Task 2 runs before Tasks
  7/8 finish, the links are still valid since this is a monorepo path reference, not a build
  dependency"). `docs/hardware_requirements.md` (Task 7) is intentionally **not** linked directly
  from the README — the brief's outline only calls for a Quick Start pointer to
  `docs/getting_started.md`, which will itself link to the hardware doc.
- Confirmed the CI badge URL (`unit-tests.yml`) and License badge still resolve — the workflow
  file exists at `.github/workflows/unit-tests.yml`.
- Grepped the finished doc for every banned old-doc term (`main.ino`, `arduino`, `75.byte`,
  `EepromManager`, `EEPROM_Manager`, `MacAddress.h`, `src/Mesh/`, `12-byte`, `WROOM`) — the only
  hits are the two intentional "no Arduino IDE/`arduino-cli`" statements.

## Self-assessment

Factual accuracy: every numeric/named claim in the doc (pin numbers, constant defaults, error-code
digit values, CPU frequencies, message struct size, test commands) was checked against live source
in this session via `grep`/`Read`, not transcribed uncritically from the research file or the old
README. Two corrections were made beyond what the research/brief explicitly flagged: the missing
`CRYPTO=7` error-type digit and the sub-code range (decimal 0–9, not hex 0–F), and the CPU-scaling
wording (dynamic range vs. old doc's implied fixed value) — both independently verified against
current source, not guessed.

## Concerns

None blocking. One judgment call worth flagging to the controller: I deviated from the brief's
literal phrasing "target chip `esp32` (per `sdkconfig.defaults`'s `CONFIG_IDF_TARGET`)" because
that specific claim is false against current source — `CONFIG_IDF_TARGET` isn't in
`sdkconfig.defaults`, only in the gitignored generated `sdkconfig`. I sourced the same true fact
(target is `esp32`) via the accurate mechanism (`idf.py set-target esp32`) instead of citing a file
that doesn't contain what the brief claims it contains.
