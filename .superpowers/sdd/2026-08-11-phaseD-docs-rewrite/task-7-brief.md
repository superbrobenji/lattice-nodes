### Task 7: `docs/hardware_requirements.md` (NEW)

**Files:** Create `docs/hardware_requirements.md`

**Research:** `phaseD-research-adapter-hardware.md` (has the complete pin table, pull-config analysis, and wiring-guide conclusions already worked out — this task's job is presentation, including Mermaid diagrams, not new research)

**Content outline:**
1. **Bill of materials** — ESP32 dev board (the research doesn't name a specific SKU; note generically "any ESP32 dev board with the pins below broken out" unless a specific board is referenced elsewhere in the repo — check `README.md`'s old Requirements table, which said "ESP32-WROOM-DA (or compatible ESP32)", verify this is still the intended target board by checking `firmware/sdkconfig.defaults`'s `CONFIG_IDF_TARGET` — the research confirms target is `esp32`, generic enough that WROOM-DA or similar variants apply), 2x LED (any color, current-limiting resistor required — not managed by firmware), 2x momentary push-button, 1x PIR motion sensor module (3.3V logic — flag the level-shifter caveat from the research for 5V-logic modules), 1x TM1637 4-digit 7-segment display breakout, USB cable, breadboard/wiring.
2. **Pin assignment table** — the exact table from the research (`RED_LED_PIN=33`, `GREEN_LED_PIN=26`, `CONFIG_BUTTON_PIN=32`, `RESET_BUTTON_PIN=25`, `SEVSEG_DATA_PIN=23`, `SEVSEG_CLK_PIN=22`, `PIR_ADAPTER_DEFAULT_PIN=27`), each with which `project_config.h`/`AdapterFactory.h` constant it comes from.
3. **Per-component wiring, each with a Mermaid diagram.** Use `graph LR` or `graph TD` flowchart syntax (not a true schematic notation — Mermaid doesn't do circuit symbols — but a clear labeled-node connection diagram is the right fit here): one diagram each for (a) the two LEDs + their current-limiting resistors to GPIO 33/26, (b) the two buttons to GPIO 32/25 (note: internal pull-down, active-HIGH — button connects the pin to 3.3V when pressed), (c) the PIR sensor to GPIO 27 (note: internal pull-up), (d) the TM1637 display's DIO/CLK to GPIO 23/22 plus its VCC/GND. Example node/edge style: `ESP32_GPIO33["ESP32 GPIO33"] --> R["220Ω resistor"] --> LED_Red["Red LED (+)"] --> GND`.
4. **No external pull resistors needed** — state this explicitly and why (all pull config is internal, set once in `main.cpp`'s `initDrivers()`), citing the specific 3 `gpio_config_t` groups from the research (output group, button input group, PIR input group) so a technical reader can verify against source if they want to.
5. **Power** — ESP32 dev boards typically run off USB 5V with an onboard 3.3V regulator; note that the TM1637/PIR breakouts should be checked against their own datasheet for VCC tolerance (3.3V vs 5V) since firmware has no control over that.

**Must get right:** Every pin number must match `project_config.h`/`AdapterFactory.h` exactly (cross-check against `phaseD-research-adapter-hardware.md`'s table, which was grep-verified). Do not invent a specific PIR/display part number the research didn't confirm — describe generically where the research doesn't name a specific SKU.

- [ ] Write the new doc per the outline above, including 4 Mermaid wiring diagrams.
- [ ] Self-check: render each Mermaid block mentally (or paste into a Mermaid live-editor if available) to confirm valid syntax before committing — a broken Mermaid fence renders as a raw code block on GitHub, not a diagram.
- [ ] Commit: `git add docs/hardware_requirements.md && git commit -m "docs(phaseD): add hardware_requirements.md with pinout + wiring diagrams"`

---

