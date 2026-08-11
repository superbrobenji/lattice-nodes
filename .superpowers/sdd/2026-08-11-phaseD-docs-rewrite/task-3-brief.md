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

