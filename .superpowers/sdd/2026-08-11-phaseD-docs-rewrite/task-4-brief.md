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

