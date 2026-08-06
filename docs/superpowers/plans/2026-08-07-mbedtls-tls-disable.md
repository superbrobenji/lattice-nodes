# mbedtls TLS Disable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `=n` mbedtls trims real: `CONFIG_MBEDTLS_TLS_DISABLED=y` + `CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n`, completing item KK (Curve25519-only), with truthful comments.

**Architecture:** Config-only change to `firmware/sdkconfig.defaults`, staged as two independently-revertable flag commits plus a comment-truth pass. Each stage gates on a clean ESP-IDF build + resolved-sdkconfig greps; Step B additionally gates on a size decision rule (revert if the image grows).

**Tech Stack:** ESP-IDF 5.5.1 (`$HOME/esp/esp-idf`), Kconfig, host GoogleTest suite.

**Spec:** `docs/superpowers/specs/2026-08-07-mbedtls-tls-disable-design.md` — read before starting.

## Global Constraints

- Branch: `feat/mbedtls-tls-disable` (exists, spec committed). Only file modified: `firmware/sdkconfig.defaults`.
- Build parallelism capped: `idf.py build -- -j2`; host `cmake --build build -j2`, `ctest --parallel 1`. Full-parallel OOMs this machine.
- Clean target build sequence (from `firmware/`, after `source $HOME/esp/esp-idf/export.sh`): `rm -rf build sdkconfig managed_components dependencies.lock && idf.py reconfigure && idf.py build -- -j2`. Builds take ~5-8 min — do not kill.
- Target build needs `firmware/main/config/master_pubkey_pin.h`: if absent, generate a throwaway via `python3 tools/gen_master_pubkey_pin.py` conventions used in Phase J (random 32-byte key, MAC `aa:bb:cc:dd:ee:ff`); it is gitignored — never commit it. Remove it (`rm -f`) before any host-test grep/run.
- Do NOT commit `firmware/sdkconfig`, `managed_components/`, `dependencies.lock` (untracked artifacts).
- Size baseline: **685,099 B** total image (Phase J final, `a3eb997`). No reduction target; Step B reverts if total grows above its Step A predecessor.
- Commits: conventional style, end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## File Structure

- Modify `firmware/sdkconfig.defaults` only: Step A line into the mbedtls trim block (~line 59), Step B line into the WiFi section (next to `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`, ~line 93), comment rewrites of the blocks at lines 45-54 and 121-125.

---

### Task 1: Step A — CONFIG_MBEDTLS_TLS_DISABLED=y

**Files:**
- Modify: `firmware/sdkconfig.defaults:59` (append after `CONFIG_MBEDTLS_TLS_ENABLED=n`)

**Interfaces:**
- Consumes: nothing.
- Produces: Task 2 builds on this commit; Task 3 needs this task's recorded `idf.py size` total.

- [ ] **Step 1: Add the flag.** Directly after the line `CONFIG_MBEDTLS_TLS_ENABLED=n` insert:

```
# TLS-disable follow-up: select the MBEDTLS_TLS_MODE choice's "None" option,
# ending the choice-default's TLS_SERVER/TLS_CLIENT -> TLS_ENABLED selects —
# this makes TLS_ENABLED=n above real (it was advisory before; see spec
# docs/superpowers/specs/2026-08-07-mbedtls-tls-disable-design.md).
CONFIG_MBEDTLS_TLS_DISABLED=y
```

- [ ] **Step 2: Clean build.**

```bash
cd firmware
source $HOME/esp/esp-idf/export.sh
rm -rf build sdkconfig managed_components dependencies.lock
idf.py reconfigure && idf.py build -- -j2
```
Expected: `Project build complete.` If it fails inside `esp-tls`/`esp_http_*`/`mqtt`/`*_provisioning`/`arduino-esp32` (TLS consumers compiled against a TLS-disabled mbedtls): report BLOCKED with the exact first error — do NOT patch components; the spec's containment is dropping this flag, and that decision goes back to the controller.

- [ ] **Step 3: Verify resolved config.**

```bash
grep -E "^CONFIG_MBEDTLS_TLS_ENABLED=y|^CONFIG_MBEDTLS_SSL_PROTO.*=y|^CONFIG_MBEDTLS_TLS_SERVER=y|^CONFIG_MBEDTLS_TLS_CLIENT=y" sdkconfig
```
Expected: empty output (all TLS symbols unset). Also confirm still present (wrapper deps):

```bash
grep -c "^CONFIG_MBEDTLS_ECP_C=y" sdkconfig; grep -c "^CONFIG_MBEDTLS_ECDH_C=y" sdkconfig
```
Expected: `1` and `1`.

- [ ] **Step 4: Measure.**

```bash
idf.py size
```
Record the total-image number (call it SIZE_A) in your report with the full size table. Reference: predecessor baseline 685,099 B; expect ≈ equal, any reduction is bonus, growth is NOT a revert trigger for Step A (only Step B has the revert rule) but flag it prominently if > +1 KB.

- [ ] **Step 5: Commit.**

```bash
git add firmware/sdkconfig.defaults
git commit -m "feat(tls-disable): CONFIG_MBEDTLS_TLS_DISABLED=y — end TLS_MODE choice force-select

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Step B — CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n + keep/revert decision

**Files:**
- Modify: `firmware/sdkconfig.defaults:90-93` region (append after `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`)

**Interfaces:**
- Consumes: Task 1's commit + SIZE_A.
- Produces: the shipped end-state (B kept or B reverted) + SIZE_B for Task 3's comment rewrite and PR body. Report MUST state which end-state shipped.

- [ ] **Step 1: Add the flag.** Directly after `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n` insert:

```
# TLS-disable follow-up step B: stop esp_wifi selecting mbedtls crypto
# (AES_C/ECP_C/ECDH_C/ECDSA_C/CMAC_C/SECP256R1 all force-selected =y by
# ESP_WIFI_MBEDTLS_CRYPTO, default y). Requires OWE_STA off too — WPA3 OWE
# (default y) independently selects ESP_WIFI_MBEDTLS_CRYPTO, and OWE only
# matters for open-AP association, which this firmware never does (pure
# ESP-NOW; WPA3_SAE/SAE_PK already =n above). wpa_supplicant falls back to
# its internal crypto implementations — near-dead code here (no AP
# association, no WPA handshake at runtime). Completes item KK: resolved
# config carries exactly one ECP curve (CURVE25519).
CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA=n
CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n
```

Additionally (attempt-3 amendment), replace the line `CONFIG_MBEDTLS_AES_C=n` with:

```
# AES_C must stay =y: ESP-IDF's mbedtls port hardcodes MBEDTLS_CTR_DRBG_C
# (not Kconfig-toggleable), and CTR_DRBG requires AES_C — the old =n here
# was always a dead letter (force-selected =y by esp_wifi chains until this
# follow-up; consistency-error in check_config.h if actually applied).
# GCM/CCM/HARDWARE_AES below remain real, effective trims.
CONFIG_MBEDTLS_AES_C=y
```

- [ ] **Step 2: Clean build** (same sequence as Task 1 Step 2). Expected: `Project build complete.` If it fails inside `wpa_supplicant` (internal-crypto fallback never built here): report BLOCKED with the exact first error — containment is dropping this flag, controller decides.

- [ ] **Step 3: Verify resolved config.**

```bash
grep -E "^CONFIG_MBEDTLS_AES_C=y|^CONFIG_MBEDTLS_ECDSA_C=y|^CONFIG_MBEDTLS_CMAC_C=y|^CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y|^CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=y" sdkconfig
```
Expected: empty.

```bash
grep -c "^CONFIG_MBEDTLS_ECP_DP_.*=y" sdkconfig
grep -c "^CONFIG_MBEDTLS_ECP_C=y" sdkconfig; grep -c "^CONFIG_MBEDTLS_ECDH_C=y" sdkconfig
```
Expected: `1` (CURVE25519 only), then `1` and `1` (wrapper deps survive on their Kconfig defaults).

- [ ] **Step 4: Measure + decide.**

```bash
idf.py size
```
Record SIZE_B with the full table. **Decision rule (spec-mandated): if SIZE_B > SIZE_A, revert Step B** — `git checkout -- firmware/sdkconfig.defaults` is wrong here since Step 1 is uncommitted; instead remove the inserted block (comment + flag line), re-run the clean build + `idf.py size` to confirm the tree matches SIZE_A again, and record the grown SIZE_B number as evidence for the revert. If SIZE_B ≤ SIZE_A: keep.

- [ ] **Step 5: Commit (only if kept).**

```bash
git add firmware/sdkconfig.defaults
git commit -m "feat(tls-disable): ESP_WIFI_MBEDTLS_CRYPTO=n — complete KK, wpa_supplicant internal crypto

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
If reverted: no commit; report the measured numbers and the revert decision.

---

### Task 3: Truthful comments + host suite + PR

**Files:**
- Modify: `firmware/sdkconfig.defaults:45-54` (AES/TLS comment block) and `:121-125` (KK comment block)

**Interfaces:**
- Consumes: Task 2's shipped end-state (B kept vs reverted) + SIZE_A/SIZE_B.
- Produces: pushed branch + open PR.

- [ ] **Step 1: Rewrite the AES/TLS comment block.** Replace lines 45-54 (from `# AES/GCM/CCM/TLS below are a SEPARATE...` through `# out of Phase J scope.`) with the variant matching the shipped end-state:

**If Step B KEPT:**
```
# AES/GCM/CCM/TLS below are a SEPARATE, pre-existing Phase G/Phase A size trim
# (no TLS/HTTPS use at all, independent of which library E2E crypto calls).
# These =n lines are EFFECTIVE as of the TLS-disable follow-up (spec
# docs/superpowers/specs/2026-08-07-mbedtls-tls-disable-design.md): both
# Kconfig chains that silently forced them back to =y are ended —
# CONFIG_MBEDTLS_TLS_DISABLED=y (below) stops the MBEDTLS_TLS_MODE choice's
# TLS_ENABLED select, and CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n (WiFi section)
# stops esp_wifi's AES_C/ECDSA_C/CMAC_C/SECP256R1 selects. (Phase J's
# earlier comment attributed ECDSA_C/SECP256R1 to the TLS chain — the
# actual source was ESP_WIFI_MBEDTLS_CRYPTO; corrected here.)
```

**If Step B REVERTED:**
```
# AES/GCM/CCM/TLS below are a SEPARATE, pre-existing Phase G/Phase A size trim
# (no TLS/HTTPS use at all, independent of which library E2E crypto calls).
# CONFIG_MBEDTLS_TLS_DISABLED=y (below) makes TLS_ENABLED=n real (spec
# docs/superpowers/specs/2026-08-07-mbedtls-tls-disable-design.md). NOTE:
# AES_C/ECDSA_C/CMAC_C/SECP256R1 remain force-selected =y by
# ESP_WIFI_MBEDTLS_CRYPTO (esp_wifi Kconfig, default y, also selected by
# WPA3 OWE_STA) — NOT the TLS chain as Phase J's earlier comment said;
# corrected here. Ending those selects is INFEASIBLE on this ESP-IDF
# version: AES_C can never be off (the port hardcodes MBEDTLS_CTR_DRBG_C,
# which requires it), and ESP_WIFI_MBEDTLS_CRYPTO=n fails at link
# ("undefined reference to sha1_vector" — wpa_supplicant's internal-crypto
# fallback is broken upstream). See the follow-up spec's final-outcome note.
```

- [ ] **Step 2: Rewrite the KK comment block.** Replace lines 121-125 (from `# Phase J item KK (revived):...` through `# ...stripped by LTO/gc-sections).`) with:

**If Step B KEPT:**
```
# Phase J item KK (revived): only Curve25519 is used (X25519 E2E keys).
# ESP-IDF's default enables every NIST/Brainpool curve in mbedtls — dead
# flash. EFFECTIVE as of the TLS-disable follow-up: with
# ESP_WIFI_MBEDTLS_CRYPTO=n the SECP256R1/ECDSA_C force-selects are gone;
# the resolved config carries exactly one curve (CURVE25519).
```

**If Step B REVERTED:**
```
# Phase J item KK (revived): only Curve25519 is used (X25519 E2E keys).
# ESP-IDF's default enables every NIST/Brainpool curve in mbedtls — dead
# flash here (nothing references them; note SECP256R1 + ECDSA_C get
# force-selected =y by ESP_WIFI_MBEDTLS_CRYPTO — see comment above — but
# their unreferenced code is stripped by LTO/gc-sections).
```

- [ ] **Step 3: Verify comments against reality.** Every claim in the rewritten comments must match the resolved `firmware/sdkconfig` from Task 2's final build — re-run Task 2 Step 3's greps and check each sentence. No comment may claim a state the resolved config contradicts.

- [ ] **Step 4: Confirm comment-only diff + rebuild parse check.**

```bash
git diff firmware/sdkconfig.defaults | grep "^[+-]" | grep -v "^[+-][+-]" | grep -v "^[+-]#"
```
Expected: empty (all changed lines are comments). Then a plain `idf.py build -- -j2` (incremental) to prove defaults still parse: expected `Project build complete.`

- [ ] **Step 5: Host suite regression gate.**

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && rm -rf build && cmake -B build && cmake --build build -j2 && cd build && ctest --parallel 1
```
Expected: `100% tests passed out of 298`.

- [ ] **Step 6: Commit + push + PR.**

```bash
git add firmware/sdkconfig.defaults
git commit -m "docs(tls-disable): truthful force-select comments — correct ECDSA/SECP256R1 attribution

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push -u origin feat/mbedtls-tls-disable
gh pr create --base main --title "TLS disable: make mbedtls =n trims real (Phase J follow-up)" --body "$(cat <<'EOF'
Config-only follow-up to Phase J (#94). Makes the advisory `=n` lines in `sdkconfig.defaults` real:

- `CONFIG_MBEDTLS_TLS_DISABLED=y` — ends the `MBEDTLS_TLS_MODE` choice's `TLS_ENABLED` force-select.
- `CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n` — ends esp_wifi's `AES_C/ECDSA_C/CMAC_C/SECP256R1` selects; wpa_supplicant uses its internal crypto (near-dead code in this pure-ESP-NOW firmware). <ADJUST IF REVERTED: state the flag was measured to grow the image and was not shipped.>
- Comment truth pass: corrects Phase J's misattribution of the ECDSA/SECP256R1 force-select (source is `ESP_WIFI_MBEDTLS_CRYPTO`, not the TLS chain).

Item KK completed for real: resolved config carries exactly one ECP curve (CURVE25519). <ADJUST IF REVERTED.>

Spec: `docs/superpowers/specs/2026-08-07-mbedtls-tls-disable-design.md`

## Size
| | Total image |
|---|---|
| Phase J baseline (`a3eb997`) | 685,099 B |
| Step A (TLS_DISABLED) | <SIZE_A> B |
| Step B (WIFI_MBEDTLS_CRYPTO=n) | <SIZE_B or "reverted, measured <SIZE_B>"> B |

Config-hygiene change — no reduction target; runtime coverage deferred to next physical flash (accepted in spec).

## Tests
298/298 host tests; resolved-sdkconfig greps in task reports.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
Fill `<SIZE_A>`/`<SIZE_B>` from Tasks 1-2 and apply the `<ADJUST IF REVERTED>` edits per the shipped end-state before running. Report the PR URL. Do not watch CI beyond one `gh pr checks` snapshot — the queue-hang issue is known; do not merge, do not cancel runs.

---

## Verification Summary (spec acceptance → plan coverage)

1. Clean build per step — Task 1 Step 2, Task 2 Step 2.
2. Resolved-config assertions (TLS unset; AES/ECDSA/CMAC/SECP256R1 unset; curve count 1; ECP_C/ECDH_C survive) — Task 1 Step 3, Task 2 Step 3, re-checked Task 3 Step 3.
3. 298/298 host tests — Task 3 Step 5.
4. Size recorded per step + Step B revert rule — Task 1 Step 4, Task 2 Step 4.
5. Single PR to main — Task 3 Step 6.
