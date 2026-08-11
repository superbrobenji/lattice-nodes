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

