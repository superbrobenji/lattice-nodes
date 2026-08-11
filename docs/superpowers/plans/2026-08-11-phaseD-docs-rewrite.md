# Phase D — Docs Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite all 6 severely stale lattice-nodes docs and add 2 new ones (hardware requirements + a non-technical getting-started guide), grounded entirely in grep-verified current source — not incremental edits to the old prose, which describes a different codebase (deleted files/APIs, wrong build system, a wire schema missing years of protocol history).

**Architecture:** 8 independent doc-writing tasks, each backed by exhaustive research already completed and saved to the scratchpad (paths given per task below — read the referenced file(s) first, they are the verified source of facts; do not re-derive from memory or guess). Each task also gets the specific "must be correct" facts inline in this plan, in case the scratchpad becomes unavailable.

**Tech Stack:** Markdown, Mermaid diagrams (GitHub renders these natively — no external tooling).

## Global Constraints

- **Accuracy bar, not behavior-preservation.** There is no code to keep unchanged here — the bar is "every concrete claim (file path, class/function name, wire field, pin number, numeric constant) matches current source or the verified research." A well-written but wrong doc is worse than the stale one it replaces.
- **No code changes.** This phase is docs-only. If a task's research surfaces something that looks like a real bug (e.g. `OP_LED_*`'s payload-shape inconsistency, error-code collisions), document it as a known gap/inconsistency — do not fix the code.
- **Cross-repo scope, per repo.** This plan covers `lattice-nodes` only. Where a doc needs to describe `lattice-hub`/`lattice-protocol`, describe them accurately but briefly (an "ecosystem overview," not a deep dive) — `lattice-hub` and `lattice-protocol` get their own separate docs efforts in their own repos later.
- **Mermaid diagrams embedded directly** in the relevant `.md` files (GitHub renders `​```mermaid` fences natively).
- **File paths in code examples must be copy-pasteable and correct** — every path cited in these docs must be verified to exist in the current tree (the whole reason for this phase existing is that the old docs cited deleted paths).

## Research Files (read before writing — absolute paths, already verified against current source)

- `/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-mesh.md` — full `mesh/` subsystem map (22 files, ~16 collaborators)
- `/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-adapter-hardware.md` — `adapter/` + `hardware/` current API, pin assignments, "how to add an adapter" walkthrough, wiring-guide conclusions
- `/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-persistence-crypto-error.md` — `persistence/eeprom/` (8 files), `crypto/`, `network/`, exhaustive error-code registry (28+7 call sites), `logging/`, `app/`
- `/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-wire-protocol.md` — full `mesh_message` field list, message types, opcodes, enrollment/JOIN_ACK flow, E2E crypto scope, cross-checked against the `lattice-protocol` submodule
- `/private/tmp/claude-501/-Users-benji-projects-personal/ba8f3ff2-d8d5-45ac-b1f4-8c15c9b1ed6c/scratchpad/phaseD-research-build-flash-config.md` — real `idf.py build`/`size` output, `project_config.h` full reference, first-boot provisioning flow, LED/display status meanings, button timings, the `master_pubkey_pin.h` build prerequisite

**Ecosystem overview (for Task 2/6/8 — no separate file, inline here):** `lattice-hub` is the Go server the master node talks to over USB serial (115200 baud) — `server/orchestrator/` handles serial comms + the mesh protocol + a REST API + Kafka producer; `server/dashboard/`+`server/artist-portal/` are React admin/artist UIs; `server/sidecar/` is a health/monitoring service. Run via `docker compose -f server/docker-compose.yml up -d` (needs `server/.env` with `API_KEY`/`ADMIN_KEY` set) or the hardware-free `docker-compose.stub.yml` stack (a `mesh-sim` TCP stand-in for a real serial-attached master). `lattice-protocol` is the shared wire-format source of truth — a Go struct with dual `c:`/`proto:` tags in `message/message.go` that codegens both the C headers `lattice-nodes` vendors as a git submodule and the Go types `lattice-hub` imports as a module; both are currently pinned to `v0.6.0`. Both `lattice-hub` and `lattice-protocol`'s own READMEs are current/accurate as of this research (2026-08-11) — safe to cite directly if a doc needs to point a reader there.

## Sequencing

```
Task 1 (REFACTORING_GUIDE.md) — alone, first (other docs link to its module map)
  then, in parallel (worktree-safe, 7-way, zero file overlap):
Task 2 (README.md)
Task 3 (adapter_development_guide.md)
Task 4 (error_codes.md)
Task 5 (memory_usage.md)
Task 6 (server_requirements.md)
Task 7 (hardware_requirements.md, new)
Task 8 (getting_started.md, new)
```

Given this session's `isolation:"worktree"` auto-provisioning has an unreliable track record (has silently branched from a stale base repeatedly), **create worktrees manually** (`git worktree add <path> <verified-base-sha> -b <branch>`) rather than relying on it, and copy this plan file's task briefs into each worktree explicitly if they don't travel via `git worktree add` (only tracked files do).

---

### Task 1: `REFACTORING_GUIDE.md`

**Files:** Modify `REFACTORING_GUIDE.md` (full rewrite)

**Research:** `phaseD-research-mesh.md` (primary), `phaseD-research-adapter-hardware.md`, `phaseD-research-persistence-crypto-error.md`

**Content outline:**
1. **Design Principles** — Tiger Style (static allocation, WDT, assertions), "encapsulation yes, inheritance sparingly" (the umbrella's own established convention — cite the `Adapter`/`PirAdapter`/`SerialAdapter` hierarchy as the one deliberate inheritance use, and the `mesh_table.h` free-function pattern as the deliberate non-inheritance choice for the 5 MAC-keyed tables), no heap after boot.
2. **Module Map — `mesh/`** — the full ~16-collaborator breakdown from the research file, organized by the research's own grouping (radio/transport, peer/identity, routing, messaging/send-pipeline, security/crypto), each with its one-sentence responsibility and key API. Include the `Mesh.h`/`Mesh.cpp` orchestrator section verbatim from the research (it's already well-structured as a table) — this is the single most important part of this doc, since "Mesh is now a thin orchestrator over N collaborators" is the whole point of Phase B and needs to read as concretely true, not asserted.
3. **Module Map — `adapter/` + `hardware/`** — `Adapter` base class API, `AdapterFactory`, `PirAdapter`/`SerialAdapter`, hardware driver classes — condensed from the research (full depth belongs in `adapter_development_guide.md`, this is the map-level summary).
4. **Module Map — `persistence/eeprom/`** — the 8-file split, no facade re-export, flat namespace.
5. **Module Map — `crypto/`, `network/`, `error/`, `logging/`, `app/`** — condensed one-paragraph-each from the research.
6. **"Adding a Module"** checklist (update the old doc's 6-point list to reflect current conventions: SRP, route errors through `Error.h`'s digit-based API — not the deleted legacy overload — `GpioInput`/`GpioOutput` for new single-pin drivers, reserve containers at boot, and the CMakeLists dual-registration requirement from `firmware/main/CMakeLists.txt` + `tests/CMakeLists.txt`).

**Must get right:** `Mesh.h`+`Mesh.cpp` = 863 lines total (421+442), not a single ~1382-line file. No `EepromManager` (deleted, 8 files now). No `MacAddress.h` (deleted). No `LED_ADAPTER` in the firmware-side enum (though flag that the wire protocol reserves it — see Task 6).

- [ ] Write the full rewrite per the outline above.
- [ ] Self-check: grep the new doc for every file path it cites; confirm each exists in the current tree (`find firmware/main/src -name "<name>"`).
- [ ] Commit: `git add REFACTORING_GUIDE.md && git commit -m "docs(phaseD): rewrite REFACTORING_GUIDE.md for the post-A/B/C/E architecture"`

---

### Task 2: `README.md`

**Files:** Modify `README.md` (full rewrite)

**Research:** `phaseD-research-build-flash-config.md` (project_config.h reference, LED/display meanings), ecosystem overview (this plan's header section)

**Content outline:**
1. **Overview** — what Lattice is (ESP-NOW mesh, AES/PMK link-layer + E2E AEAD payload crypto, enrollment protocol, adapter system), one paragraph.
2. **Ecosystem** — brief: this repo is the firmware; `lattice-hub` (server, USB serial) and `lattice-protocol` (shared wire format, git submodule at `firmware/main/lib/lattice-protocol`) are siblings. Link out, don't duplicate their content.
3. **Features** — update the old bullet list: ESP-NOW mesh, E2E AEAD payload encryption (X25519+ChaCha20-Poly1305, not just "AES-encrypted 16-byte mesh key" — that PMK is link-layer only, the real payload security is E2E now), enrollment protocol with TOFU master-pubkey pinning, chain-MAC route-report authentication, dual-master failover, replay protection (epoch+seq), adapter system (PIR/Serial today, LED/Relay reserved on the wire), Tiger Style engineering, seven-segment error codes, low-power CPU scaling.
4. **Architecture** — a SHORT summary (directory tree with one-line-per-directory, current paths: `firmware/main/src/{mesh,adapter,hardware,persistence/eeprom,crypto,network,error,logging,app}/`) — link to `REFACTORING_GUIDE.md` for the full module map, don't duplicate it here.
5. **Requirements** — ESP-IDF v5.5.1 (per `dependencies.lock`), target chip `esp32` (per `sdkconfig.defaults`'s `CONFIG_IDF_TARGET`), no more Arduino IDE/arduino-cli.
6. **Quick Start** — brief pointer to `docs/getting_started.md` for the full walkthrough; here just: clone + submodule init, `idf.py build`, `idf.py -p <PORT> flash`.
7. **`project_config.h` Reference** — table from the build-flash-config research (every constant a first-time user configures: mesh key, default peers, adapter type, WIFI_CHANNEL, dev mode, log level, hardware pins, etc.)
8. **Buttons** — config/reset hold durations from the research (5s hold, reset has a 3s confirm window).
9. **Seven-Segment Error Codes** — brief pointer to `docs/error_codes.md`.
10. **Server Integration** — pointer to `docs/server_requirements.md`.
11. **Development** — unit test / e2e test commands (these are still accurate in the old README — verify against `tests/CMakeLists.txt`/current CI workflow files before reusing verbatim, don't assume).
12. **Adding a New Adapter** — pointer to `docs/adapter_development_guide.md`.
13. **Contributing** / **License** / **Security** — largely reusable from the old README, verify `CONTRIBUTING.md`/`SECURITY.md`/`LICENSE` still exist and the summary bullets are accurate (in particular: "All errors via `src/error/Error.h`" is still true, but drop any reference to the deleted legacy `ErrorType` overload if the old README mentions it).

**Must get right:** No `main.ino`/Arduino IDE/arduino-cli anywhere. No `main/src/Mesh/Mesh.h` (old capitalized path) — current is `firmware/main/src/mesh/`. The mesh message struct is 200 bytes (protocol v5), not 75 bytes with a 12-byte data field — data is 64 bytes now (cite Task 6's research if giving field-level detail, or just link to `server_requirements.md` rather than duplicating the field table here).

- [ ] Write the full rewrite per the outline above.
- [ ] Verify the CI badge URL, LICENSE link, and every `docs/*.md` link actually resolve to files that exist (including the 2 new docs this plan creates — coordinate: if Task 2 runs before Tasks 7/8 finish, the links are still valid since this is a monorepo path reference, not a build dependency).
- [ ] Commit: `git add README.md && git commit -m "docs(phaseD): rewrite README.md for the post-A/B/C/E architecture"`

---

### Task 3: `docs/adapter_development_guide.md`

**Files:** Modify `docs/adapter_development_guide.md` (full rewrite)

**Research:** `phaseD-research-adapter-hardware.md` (has a complete, ready-to-use 10-step walkthrough already — this task is largely transcription + polish, not new research)

**Content outline:**
1. **Table of Contents** (keep the old doc's structure: adding an adapter, changing the default, architecture overview, testing).
2. **Adapter Architecture Overview** — `Adapter` base class's full current API (ctor, virtuals, the control-op dispatch table, health-report builders) from the research.
3. **Adding a New Adapter** — the research's 10-step walkthrough verbatim (directory layout, constructor pattern, `init()`/`loop()` conventions, enum registration, `AdapterFactory` registration, GPIO boot config, persistence). Use `PirAdapter` as the running example exactly as the research does.
4. **Changing the Default Adapter** — edit `DEFAULT_ADAPTER` in `project_config.h` (verify this constant name still matches current `project_config.h` — cross-check against Task 2's research if needed).
5. **Testing Your New Adapter** — host-test pattern: what a new adapter's unit test should cover, referencing an existing adapter test file as the pattern to copy (check `tests/unit/test_pir_adapter.cpp` exists and use it as the cited example).

**Must get right:** Directory convention is lowercase, no per-adapter `Adapter/` parent folder (`src/adapter/pir/PirAdapter.{h,cpp}`, not `src/Adapter/PIR_Adapter/`). Base ctor is `explicit Adapter(uint8_t pin)` — type is NOT a constructor parameter. Current `adapter_types` enum has only `UNKNOWN_ADAPTER=0, SERIAL_ADAPTER=1, PIR_ADAPTER=2` — no `LED_ADAPTER`.

- [ ] Write the full rewrite per the outline above, using the research's walkthrough as the primary source.
- [ ] Self-check: confirm `tests/unit/test_pir_adapter.cpp` (or whichever example file is cited) actually exists.
- [ ] Commit: `git add docs/adapter_development_guide.md && git commit -m "docs(phaseD): rewrite adapter_development_guide.md for current Adapter API"`

---

### Task 4: `docs/error_codes.md`

**Files:** Modify `docs/error_codes.md` (full rewrite)

**Research:** `phaseD-research-persistence-crypto-error.md` (has the complete 35-row error registry table plus TM1637 mapping — this task is primarily transcription + framing, the hard research is done)

**Content outline:**
1. **How codes work** — `TMS` 3-digit decimal (`makeErrorCode(t,m,sub) = t*100 + m*10 + (sub%10)`), current `ErrorTypeDigit` (GENERIC=1..CRYPTO=7) and `ModuleDigit` (CORE=1..HW=5) enums, exact current values from the research.
2. **Current public API** — `lattice::err::fail(ErrorTypeDigit, ModuleDigit, uint8_t sub, const char* msg)` / `fatal(...)` (same signature). **Do not describe the legacy `fail(utils::ErrorType, const char*)` overload as available — it's deleted.**
3. **Registry table** — the full 35-row table from the research (28 `firmware/main/src` call sites + 7 `main.cpp` call sites), with file:line, T/M/S, resulting code, message, trigger.
4. **Known code collisions** — call out explicitly (from the research): several distinct call sites currently produce identical codes (e.g. 621 at both `PirAdapter.cpp:29` and `Adapter.cpp:37`; 622 at two sites; 651 at two sites; 552 at two sites) — a reader decoding a code off the display can't disambiguate without the (compiled-out-by-default) log message. This is real current behavior, not a doc error.
5. **TM1637 display mapping** — how the code renders on the 4-digit display (leftmost blank, then T/M/S), and the separate coarser LED-blink-count mapping (`err_core::signalError`'s `blinkPattern()`) — both from the research.
6. **Adding a new code** — updated example using the current digit-based API (replace the old doc's example, which calls the deleted legacy overload).

**Must get right:** Every code in the registry table must match the research exactly — this is the doc's whole value. Do not paraphrase/round numbers.

- [ ] Write the full rewrite per the outline above, transcribing the research's registry table directly (don't re-derive it — the research already did the exhaustive grep).
- [ ] Self-check: spot-check 5 random rows from the final table against the actual file:line cited (open the file, confirm the `err::fail`/`fatal` call really has those T/M/S arguments).
- [ ] Commit: `git add docs/error_codes.md && git commit -m "docs(phaseD): rewrite error_codes.md with the current digit-based registry"`

---

### Task 5: `docs/memory_usage.md`

**Files:** Modify `docs/memory_usage.md` (full rewrite)

**Research:** `phaseD-research-build-flash-config.md` Part 1 (verbatim real `idf.py build`/`idf.py size`/`idf.py size-components`/`idf.py size-files` output — use these real numbers, do not estimate)

**Content outline:**
1. **Status** — replace the old "re-measurement blocked" framing entirely: measurement is unblocked (ESP-IDF has been the build system since well before this phase), and this doc now carries real numbers from a real `idf.py build` run on the current tree (cite the date).
2. **Note the one real build prerequisite** discovered during measurement: `firmware/main/config/master_pubkey_pin.h` must exist (generated via `tools/gen_master_pubkey_pin.py`) before the firmware compiles — this is a deliberate `#error` gate pinning the hub's master identity into the binary, not a bug. Cross-reference `docs/getting_started.md` (Task 8) for the full generation walkthrough rather than duplicating it here.
3. **Flash breakdown** — real numbers from `idf.py size`: Flash Code (.text) 471,364 B, Flash Data (.rodata+.appdesc) 76,628 B, total image 664,131 B, app `.bin` 0xa22b0 B (37% free in the 1 MiB `factory` partition). Include the `idf.py size-components` per-library breakdown table verbatim (WiFi/mbedTLS/ESP-IDF framework vs. `libmain.a` application code, 41,140 B).
4. **RAM breakdown** — IRAM 101,111/131,072 B (77.14%, 29,961 B free), DRAM (.bss+.data) 44,084/180,736 B (24.39%, 136,652 B free).
5. **Fixed allocations by collaborator** — re-derive from `phaseD-research-mesh.md`'s per-class member data (not the old doc's single "mesh object" framing, which predates the Phase B split): `RouteTable` (~2.25KB, master-only, allocated conditionally via `Mesh::reevaluateRouteTable()`), `E2EKeyStore` (role-conditional capacity), `PeerRegistry`, `ReplayCache`, `NeighborTable`, etc. — use the research's collaborator list to build an accurate current table (the exact per-struct byte sizes may need a quick independent check against each struct's field list in `phaseD-research-mesh.md` if not already stated there — don't fabricate numbers, compute from documented field types/counts, and if a precise byte count isn't derivable from the research, say so rather than guessing).
6. **How to re-measure** — the exact real command sequence used (`source ~/esp/esp-idf/export.sh && cd firmware && idf.py build && idf.py size`), so this doc can't silently go stale the same way again — note explicitly "re-run this after any major feature addition and update the numbers in this doc."

**Must get right:** every number in this doc must come from the actual captured build/size output in the research file, not be estimated or reused from the old doc's 2026-07-13 baseline.

- [ ] Write the full rewrite per the outline, using only real captured numbers.
- [ ] Commit: `git add docs/memory_usage.md && git commit -m "docs(phaseD): rewrite memory_usage.md with real idf.py build measurements"`

---

### Task 6: `docs/server_requirements.md`

**Files:** Modify `docs/server_requirements.md` (full rewrite)

**Research:** `phaseD-research-wire-protocol.md` (primary — this is the most detailed and most externally-consequential research file; read it in full, not just skimmed), ecosystem overview (this plan's header)

**Content outline:**
1. **Topology overview** — master↔server over USB serial (115200 8N1), server never speaks ESP-NOW directly. Note this repo's sibling `lattice-hub` is the reference server implementation, `lattice-protocol` is the shared schema source (both from the ecosystem overview).
2. **CRITICAL: two different wire schemas** — lead with §0.1 of the research: the server does NOT receive the raw 200-byte `mesh_message` C struct. It receives a *different*, hand-maintained nanopb schema (`firmware/main/src/mesh/serialization/mesh.pb.h`) over the framed serial link. Explain both schemas exist and why (RF mesh protocol vs. serial transport protocol), and that they are not identical (the serial schema's `routeLen`/`routePath`/`authTag` fields exist in the .proto but are never populated by `SerialFraming::encode()`).
3. **Serial framing** — 2-byte LE length prefix + nanopb payload, 256-byte max, from research §7.
4. **Wire schema (the one the server actually implements)** — the `mesh.pb.h` field list from research §7's bullet, with each field's purpose.
5. **Message types** — the 6-row table from research §2, with corrected directions (`MASTER_BEACON` never crosses serial; `JOIN_ACK`/`SERIAL_CMD_BROADCAST` are serial-only, translated by the master before RF transmission — do not describe them as relayed verbatim).
6. **Adapter types** — research §3's table: wire-level `LED`(3)/`RELAY`(4) are reserved but **not implemented** in firmware today (no adapter class exists; setting a node to type 3/4 will fail at boot) — flag this explicitly so the server team doesn't build against unimplemented types without knowing.
7. **Control opcodes** — research §4's full table, including the explicit caveats (OP_LED_*'s inconsistent/unverified payload shape, OP_ROUTE_REPORT's currently-empty payload, OP_COMMAND_ACK never emitted). Include the `SERIAL_CMD_BROADCAST` routing semantics from §5 (target_mac_address as broadcast/unicast discriminator, `data[1..7]` as the actual unicast destination for CONFIG_SET/NODE_ID_SET).
8. **Enrollment flow** — the exact 4-part flow from research §6 (node→server request, server→master JOIN_ACK required fields, master→node rebuilt JOIN_ACK, node-side verification steps) — this is a protocol contract the server MUST implement correctly (fingerprint field, TOFU implications) so be precise, use the research's exact field-by-field table.
9. **What the server does NOT need to implement** — E2E AEAD crypto (research §8: zero crypto on the server side, it always sees plaintext) and chain-MAC route verification (research §9: route data never reaches the server today) — state this clearly so the server team doesn't over-build.
10. **Dual-master** — research §9's dual-master section: two independent physical masters, server must track both masters' MAC+pubkey identities and supply secondary-master fields in JOIN_ACK, plus the known cross-master pubkey-sync gap.

**Must get right:** This is the doc most likely to cause real integration bugs if wrong. Every field name, opcode value, and message-type direction must match the research exactly — do not paraphrase field lists into prose that loses precision.

- [ ] Write the full rewrite per the outline, transcribing the research's tables directly rather than summarizing them into lossy prose.
- [ ] Self-check: cross-reference every opcode/message-type value against `firmware/main/lib/lattice-protocol/c/opcodes.h` and `message_types.h` directly (the actual generated headers, not just the research file) as a final independent verification pass, since this doc's accuracy matters more than any other in this phase.
- [ ] Commit: `git add docs/server_requirements.md && git commit -m "docs(phaseD): rewrite server_requirements.md with the verified current wire protocol"`

---

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

### Task 8: `docs/getting_started.md` (NEW)

**Files:** Create `docs/getting_started.md`

**Research:** `phaseD-research-build-flash-config.md` (primary — has the exact command sequence, the `master_pubkey_pin.h` prerequisite, the full `project_config.h` reference, first-boot provisioning flow, and LED/display status meanings already worked out), ecosystem overview (this plan's header, for the "you'll also need a server" framing)

**This is the most important new doc per the user's explicit request: extremely thorough, written for someone with zero prior ESP32/embedded experience. Every command shown verbatim. Every expected output described. Do not assume familiarity with terminals, git, or embedded toolchains beyond basic copy-paste ability.**

**Content outline:**
1. **What you'll end up with** — one paragraph, plain language: a physical ESP32 flashed with firmware that either senses motion (PIR) or bridges to a server (master), working as part of a mesh. Note that a fully working system also needs `lattice-hub` running somewhere (link out, brief — this is a firmware-only guide, getting the hub running is out of scope here) if the reader intends to actually operate a complete system, not just flash one node.
2. **Hardware you'll need** — pointer to `docs/hardware_requirements.md` (created by Task 7 — if this task runs before Task 7 finishes, the link is still valid, just not yet resolvable; both land in the same PR).
3. **Step 1: Install ESP-IDF** — per-OS instructions (macOS/Linux/Windows) for installing ESP-IDF v5.5.1 (the pinned version from the research). Use Espressif's official installer approach for each OS (the standard `install.sh`/`export.sh` flow on macOS/Linux, the ESP-IDF Windows Installer or `install.bat`/`export.bat` on Windows) — describe the commands, and describe what a successful `idf.py --version` check should print.
4. **Step 2: Clone the repo and initialize the submodule** — `git clone`, `cd lattice-nodes`, `git submodule update --init --recursive` (confirmed necessary per the research's `.gitmodules` check) — explain what a submodule is in one sentence for a non-technical reader, and what "success" looks like (the `firmware/main/lib/lattice-protocol/` directory should be populated, not empty).
5. **Step 3: Generate your master-pubkey pin file** — **this is the newly-discovered real prerequisite**: `firmware/main/config/master_pubkey_pin.h` must exist before the build will succeed (a deliberate `#error` gate). Walk through `tools/gen_master_pubkey_pin.py` exactly as the research demonstrates (the research generated a throwaway key for its own build — for a real deployment, explain this needs to correspond to the actual hub server's master identity; for a first-time "just get it building" experience, note a random/throwaway key works for firmware compilation and local testing, but must be regenerated for real deployment against a real hub). Be explicit this file is gitignored and per-deployment, not something to commit.
6. **Step 4: Configure `project_config.h`** — the full constant-by-constant reference from the research (mesh key generation command, default peers, `DEFAULT_ADAPTER`, `WIFI_CHANNEL`, dev mode, log level) — for a first-time single-node bring-up, tell the reader which constants they can leave at defaults and which they must change (mesh key, in particular — walk through generating one).
7. **Step 5: Build** — `idf.py set-target esp32`, `idf.py build`, describe what success looks like (build progress output, final "Project build complete" message, binary size summary) and what to do if it fails (common early failure: forgetting step 2 or step 3 — point back to those steps).
8. **Step 6: Flash** — connect the board via USB, find the serial port (per-OS: `/dev/ttyUSB0` or `/dev/cu.usbserial-*` on macOS/Linux, `COMx` on Windows — explain how to find it, e.g. `ls /dev/tty.*` before/after plugging in on macOS, Device Manager on Windows), `idf.py -p <PORT> flash`, describe expected output (flashing progress bars, "Hard resetting via RTS pin" success message).
9. **Step 7: First boot — provisioning** — `idf.py -p <PORT> monitor` to watch serial output, what an unenrolled node prints (`LATTICE_PUBKEY:<64 hex chars>` — exact format from the research), and what to do with it (copy it, this is what gets registered with the hub server for enrollment — brief pointer to `server_requirements.md`'s enrollment section for the technical detail, since actually approving enrollment happens server-side).
10. **Step 8: Understanding the LEDs and display** — the full LED blink-pattern table from the research (startup, activity/data-received pulse, role-toggle confirmation, reset-arm, reset-confirm, error-code counts 1-8, fatal-fallback pattern) and seven-segment display states (dashes-while-unenrolled, node-ID-when-enrolled, decimal-point-if-master) — written for a non-technical reader ("if you see the green LED blink twice quickly, that means...").
11. **Step 9: Using the buttons** — config button (5s hold → toggle master/node role) and reset button (5s hold to arm, then hold again within 3s to confirm a full factory wipe) — exact timings from the research, written as a plain "how to" with the practical consequence of each action spelled out (role toggle restarts the device; wipe erases everything including your keys).
12. **Troubleshooting** — a short FAQ: "build fails with a `master_pubkey_pin.h` error" (did you complete Step 3?), "can't find my serial port" (driver install links per OS if the research/repo has any — check `firmware/`'s docs for CP210x/CH340 driver notes, common ESP32 USB-serial chips, and mention generically if nothing specific is documented), "node won't enroll" (pointer to checking the hub server is running and has approved the pubkey).
13. **What's next** — pointer to `README.md`'s Development section (running the test suite if the reader wants to contribute code) and a note that a pre-built-binary/simpler-flasher path is tracked as future work (link to GitHub issue #101, filed for exactly this).

**Must get right:** The `master_pubkey_pin.h` step must not be skipped or glossed over — it's a real, previously-undocumented build blocker discovered during this phase's own research, and omitting it would make this "extremely thorough for non-technical users" guide fail at the very first `idf.py build`.

- [ ] Write the new doc per the outline above — this is the longest doc in the phase, budget accordingly. Every command must be copy-pasteable exactly as written (verify each against the research file, which itself ran the real commands).
- [ ] Self-check: read through the doc pretending to be a first-time reader with zero ESP32 experience — does every step have a clear "how do I know this worked" signal? Any place a beginner could get stuck without knowing why?
- [ ] Link to GitHub issue #101 (already filed: "Pre-built release binary + simple flasher tool for non-technical setup") in the "What's next" section.
- [ ] Commit: `git add docs/getting_started.md && git commit -m "docs(phaseD): add getting_started.md, a thorough non-technical build-from-source guide"`

## Self-Review Notes

- **Spec coverage:** all 8 docs from the design spec's scope table are covered (6 rewrites + 2 new).
- **Placeholder scan:** every task has a concrete content outline backed by a specific research file, not a vague "write good docs" instruction. The "must get right" callouts per task name the specific facts most likely to be gotten wrong if an implementer works from memory instead of the research.
- **Cross-task consistency:** Task 2 (README) and Task 1 (REFACTORING_GUIDE) both describe architecture at different depths — confirmed the outline has README link out rather than duplicate Task 1's module map. Task 6 (server_requirements) and Task 8 (getting_started) both touch enrollment — confirmed the outline has Task 8 link to Task 6 for the technical detail rather than duplicating the field-level flow.
- **Known limitation flagged, not silently dropped:** several real code inconsistencies surfaced during research (error-code collisions, OP_LED's unverified payload shape, route-report data never reaching the server, the dual-master pubkey-sync gap) are explicitly called out as "document as known gap" per the Global Constraints — none are silently smoothed over into confident-sounding-but-wrong prose.
