# Task 7 Report — `docs/hardware_requirements.md` (new)

**Status:** DONE

**Commit:** `d06b5b4` on branch `phaseD-task7-manual`
("docs(phaseD): add hardware_requirements.md with pinout + wiring diagrams")

## What was done

Created `docs/hardware_requirements.md` from scratch, per the brief's outline, using
`/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-adapter-hardware.md`
as the sole source of technical facts (pin numbers, pull-config groups, wiring-guide
conclusions) — no new research, presentation only, as instructed.

1. **Bill of materials** — ESP32 dev board (generic: "ESP32-WROOM-DA (or compatible)",
   any board exposing the pins below, target chip `esp32` per
   `firmware/sdkconfig.defaults` / the phase D plan doc's own confirmation — no
   literal `CONFIG_IDF_TARGET=esp32` line exists in `sdkconfig.defaults` itself, so I
   cited the plan doc's confirmation of the same fact rather than a nonexistent line;
   see Concerns below), 2x LED + 2x ~220Ω resistor, 2x momentary button, 1x PIR module
   (3.3V logic, with the 5V-level-shifter caveat), 1x TM1637 breakout, USB cable,
   breadboard/wiring. No specific PIR/TM1637 SKU invented.
2. **Pin assignment table** — all 7 pins transcribed exactly from the research:
   `RED_LED_PIN=33`, `GREEN_LED_PIN=26`, `CONFIG_BUTTON_PIN=32`, `RESET_BUTTON_PIN=25`,
   `SEVSEG_DATA_PIN=23`, `SEVSEG_CLK_PIN=22` (all `project_config.h` §4),
   `PIR_ADAPTER_DEFAULT_PIN=27` (`AdapterFactory.h`, called out as the one exception
   living outside `project_config.h`). Included the `RESET_BUTTON_PIN`/GPIO 35 footnote
   from the research.
3. **Four Mermaid wiring diagrams** (`graph LR`), one per component group: (a) both
   LEDs + series resistors to GPIO 33/26, (b) both buttons to GPIO 32/25 with an
   internal-pull-down/active-HIGH annotation via a dotted labeled edge, (c) PIR sensor
   to GPIO 27 with the internal-pull-up + 5V-level-shifter caveat, (d) TM1637 DIO/CLK
   to GPIO 23/22 plus VCC/GND.
4. **"No external pull resistors needed"** section citing the exact 3 `gpio_config_t`
   groups (output group / button input group / PIR input group) from the research,
   each with its `pull_up_en`/`pull_down_en` setting, so a reader can verify against
   `main.cpp`'s `initDrivers()` directly.
5. **Power** section — USB 5V + onboard 3.3V regulator, PIR/TM1637 VCC-tolerance caveat
   left to their own datasheets since firmware has no control over it.

## Mermaid syntax validation — actually rendered, not eyeballed

I did not rely on visual inspection alone. Steps taken, in
`/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/mermaid-check/`:

1. First tried `@mermaid-js/parser` (the newer langium-based parser) — it does **not**
   support the `flowchart`/`graph` grammar (only pie/gitGraph/architecture/etc.), so
   that approach was abandoned as a false lead.
2. Installed `@mermaid-js/mermaid-cli` (`mmdc`), approved and downloaded Puppeteer's
   bundled headless Chrome (`npx puppeteer browsers install chrome`), extracted each
   of the 4 fenced ```` ```mermaid ```` blocks from the finished doc into standalone
   `.mmd` files, and ran `mmdc` against each one.
3. **All 4 blocks rendered successfully** to valid SVG (`aria-roledescription="flowchart-v2"`
   on every output, ~14-21 KB each, non-error content) — this is real Mermaid-engine
   parsing + layout, run inside an actual (headless) browser, not a guess.
4. Grepped each SVG for the expected `GPIO\d+` labels (26/33, 25/32, 27, 22/23) to
   confirm the right content landed in the right diagram, not just "some diagram
   rendered."
5. Additionally rendered all 4 blocks to PNG (`mmdc ... -o blockN.png -w 1000`) and
   visually inspected each with the Read tool — all four are legible, correctly
   labeled, and match the intended node/edge structure (LEDs+resistors→GND;
   buttons with dotted internal-pull-down edges; PIR with curved OUT/GND edges;
   TM1637 with 4 parallel signal/power rows).

This was full render-verification, not eyeballing — mmdc/Puppeteer is the same
rendering engine class GitHub uses to render Mermaid fences (via its own service),
so a successful `mmdc` render is strong evidence the fence will render correctly on
GitHub too.

## Verification performed

- Cross-checked every pin number in the new doc against the research file's
  "Pin config summary" and "gpio_config_t groups" sections — all 7 pins and all 3
  pull-config groups match verbatim (no re-derivation from source, as instructed).
- Read `README.md`'s existing Requirements table (`ESP32-WROOM-DA (or compatible
  ESP32)`) and `firmware/sdkconfig.defaults` directly. `sdkconfig.defaults` does
  **not** contain a literal `CONFIG_IDF_TARGET=` line (confirmed via `grep`); the
  target-chip fact ("esp32") is asserted by the phase D plan doc
  (`docs/superpowers/plans/2026-08-11-phaseD-docs-rewrite.md`, both in its own Task 5
  outline and its Task 7 outline) rather than by a grep-able line in
  `sdkconfig.defaults` itself. I kept the doc's wording generic ("Target chip is
  `esp32`... any ESP32 dev board with the pins below broken out") to avoid overclaiming
  a specific SKU beyond what's actually in the repo, consistent with the brief's
  "don't invent a specific SKU" instruction extended to the base board too.
- Confirmed `docs/` doc style (heading level starts at `##`, not `#`) by reading
  `docs/server_requirements.md`'s opening lines, and matched it.

## Self-assessment

All 7 pin numbers, all 3 pull-config groups, and all 5 wiring-guide conclusions
(LED resistor requirement, button pull-down/active-HIGH, PIR pull-up + 5V caveat,
TM1637 no-external-pull-up, TM1637 power-datasheet caveat) are verbatim from the
grep-verified research file — no new hardware facts were invented. The 4 Mermaid
diagrams were mechanically rendered end-to-end (not just visually eyeballed) and
confirmed structurally correct via both SVG-content grep and PNG visual read.

## Concerns

- Minor, non-blocking: the brief's parenthetical said to "verify this is still the
  intended target board by checking `firmware/sdkconfig.defaults`'s `CONFIG_IDF_TARGET`"
  — that exact Kconfig line does not exist in `sdkconfig.defaults` in this worktree
  (it's presumably an ESP-IDF default, set by `idf.py set-target esp32`, not
  overridden in the checked-in defaults file). I did not treat this as blocking
  since the phase D plan doc itself already asserts the `esp32` target as a
  cross-checked fact, and the doc's own board line is written generically enough
  (any ESP32 dev board with the pins broken out) that this doesn't affect
  correctness — flagging it here in case the controller wants the plan doc's
  `CONFIG_IDF_TARGET` claim double-checked against a different file (e.g. a
  generated `sdkconfig`, not `sdkconfig.defaults`, which isn't checked into this
  worktree).
