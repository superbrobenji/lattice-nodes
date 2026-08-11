# mbedtls TLS disable + WiFi-crypto de-select (Phase J follow-up)

**Date:** 2026-08-07
**Predecessor:** Phase J (`a3eb997`, PR #94) — which documented, truthfully but passively, that several `CONFIG_MBEDTLS_*=n` lines in `sdkconfig.defaults` are silently overridden to `=y` by Kconfig `select` chains. This follow-up makes those `=n` intents real.

## Goal

Actually disable the mbedtls TLS stack and the esp_wifi mbedtls-crypto selects, completing item KK (Curve25519-only ECP) for real, and correct the force-select attribution in the config comments. Config-truth work: expected size delta ≈ 0 (the affected code is already LTO/gc-stripped); any reduction is bonus.

## Background (verified against ESP-IDF 5.5.1 Kconfig + post-Phase-J resolved sdkconfig)

Two — not one — select chains override the `=n` lines:

1. **`MBEDTLS_TLS_MODE` choice** (default `SERVER_AND_CLIENT`) → selects `MBEDTLS_TLS_SERVER`+`CLIENT` → each selects `MBEDTLS_TLS_ENABLED`. The only other selector of `TLS_ENABLED` (`esp_wifi` Enterprise, `esp_wifi/Kconfig:604`) is already inactive via `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`.
2. **`ESP_WIFI_MBEDTLS_CRYPTO`** (default `y`, `esp_wifi/Kconfig:581`) → selects `MBEDTLS_AES_C`, `MBEDTLS_ECP_C`, `MBEDTLS_ECDH_C`, `MBEDTLS_ECDSA_C`, `MBEDTLS_CMAC_C`, `MBEDTLS_ECP_DP_SECP256R1_ENABLED`. **This — not the TLS chain — is what keeps `ECDSA_C` and `SECP256R1` at `=y`.** Phase J's sdkconfig comments misattribute this to the TLS-mode chain; they must be corrected here.

Post-Phase-J resolved config confirms both flags still required: `TLS_ENABLED=y`, `SSL_PROTO_TLS1_2=y`, `AES_C=y`, `ECDSA_C=y`, `CMAC_C=y`, `SECP256R1=y`, `ESP_WIFI_MBEDTLS_CRYPTO=y`.

## Changes

One file: `firmware/sdkconfig.defaults`. Two staged steps, each its own commit, clean build, and size measurement — independently revertable.

**Step A — `CONFIG_MBEDTLS_TLS_DISABLED=y`.** The `MBEDTLS_TLS_MODE` choice symbol ("None"). Removes `TLS_SERVER`/`TLS_CLIENT`/`TLS_ENABLED` and everything gated on them (`SSL_PROTO_*`, key-exchange menu).

**Step B — `CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA=n` + `CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n`.** *(Amended during execution: first attempt proved `ESP_WIFI_MBEDTLS_CRYPTO=n` alone is a no-op — a third select chain, `ESP_WIFI_ENABLE_WPA3_OWE_STA` (default `y`), independently selects it. OWE is opportunistic encryption for open-AP **association**; this firmware never associates, and the sibling `WPA3_SAE=n`/`SAE_PK=n` trims already disable the rest of WPA3 — same risk class.)* With both lines, wpa_supplicant falls back to its internal crypto implementations — near-dead code in pure ESP-NOW. Removes the six mbedtls selects listed above.

*(Second amendment, attempt 3:)* ESP-IDF's mbedtls port **hardcodes `MBEDTLS_CTR_DRBG_C`** (absent from Kconfig entirely), and CTR_DRBG requires `MBEDTLS_AES_C` — so once no chain selects AES, the historical `CONFIG_MBEDTLS_AES_C=n` line takes effect and trips `check_config.h`'s consistency error. AES can never actually be off on this platform. Step B therefore also flips our dead-letter `CONFIG_MBEDTLS_AES_C=n` to an explicit, documented `=y` (binary unchanged — the resolved config always had `AES_C=y`). The KK completion target stands: `ECDSA_C`, `CMAC_C`, `SECP256R1` off, ECP curve count exactly 1; the AES "trim" is formally retired as impossible.

*(Final outcome, attempt 3:)* **Step B is infeasible on ESP-IDF 5.5.1 via configuration alone — not shipped.** With the selects genuinely ended and `AES_C=y` explicit, the build compiles but **fails at link**: `undefined reference to 'sha1_vector'` from `wpa_supplicant/libwpa_supplicant.a(sha1.c.obj)` — the internal-crypto fallback path is broken upstream in this component combination. Three attempts, three distinct walls (select-chain no-op → hardcoded `CTR_DRBG_C`/`AES_C` conflict → internal-crypto link failure); evidence in the task-2 report. Item KK remains incomplete (`SECP256R1`/`ECDSA_C`/`CMAC_C` stay force-selected `=y`); a real fix requires upstream/wpa_supplicant work, out of config-only scope. The shipped end-state is Step A only, and the acceptance criteria scoped to Step B do not apply.

*(Fourth amendment, final review:)* The "infeasible via configuration alone" claim was too strong. Final-review source inspection found the missing `sha1_vector` is gated on `CONFIG_CRYPTO_INTERNAL`, defined only when both `MBEDTLS_SHA1_C` and `MBEDTLS_HARDWARE_SHA` are off (wpa_supplicant CMakeLists ~:334). A config-only route therefore exists but was **REJECTED**: it surrenders hardware-SHA acceleration in the E2E HMAC/HKDF hot path for a measured-zero flash benefit, and the route is untested (further walls possible). Step B remains not-shipped as a documented decision, not an impossibility.

**Comment rewrite (final step, after the keep/revert decision).** The Phase J "advisory =n" comment blocks in `sdkconfig.defaults` are rewritten to describe whichever end-state actually ships: correct the attribution (two chains, per Background), then state the resulting reality — if both flags survive, the `=n` lines are effective and ECP curve count is truly 1; if Step B was reverted by the size rule, the comment instead names `ESP_WIFI_MBEDTLS_CRYPTO` as the remaining (deliberate) override source. Wording verified against the final resolved sdkconfig, not assumed.

## Guard rail

`ESP_WIFI_MBEDTLS_CRYPTO` also selects `ECP_C`/`ECDH_C`, which `lattice::crypto` (X25519) needs. Both default `y` in ESP-IDF's mbedtls Kconfig, so they survive the de-select — but acceptance asserts it explicitly, and a missing dependency fails the build loudly at `Crypto.h` (compile error, not silent).

## Acceptance criteria

Per step: clean `idf.py build -- -j2` (from `firmware/`, env `$HOME/esp/esp-idf/export.sh`; clean = `rm -rf build sdkconfig managed_components dependencies.lock` + `reconfigure`).

After Step B, resolved `firmware/sdkconfig` must show:
- `CONFIG_MBEDTLS_TLS_ENABLED` unset (absent or `=n`); no `CONFIG_MBEDTLS_SSL_PROTO_*=y`.
- `CONFIG_MBEDTLS_AES_C`, `ECDSA_C`, `CMAC_C`, `ECP_DP_SECP256R1_ENABLED` all unset/`=n`.
- `grep -c "^CONFIG_MBEDTLS_ECP_DP_.*=y" sdkconfig` → exactly 1 (`CURVE25519`).
- `CONFIG_MBEDTLS_ECP_C=y` and `CONFIG_MBEDTLS_ECDH_C=y` survive (wrapper dependency).

Suite + size:
- 298/298 host tests (unchanged by config, run as regression gate; pin-file trap `rm -f firmware/main/config/master_pubkey_pin.h` first).
- `idf.py size` per step, baseline **685,099 B** (Phase J final, `a3eb997`). No reduction target. **Decision rule: if Step B grows the total image, revert Step B** (keep Step A regardless, provided it builds); record all numbers.

Single PR `feat/mbedtls-tls-disable` → `main`.

## Risks

- **TLS-consuming components in the build graph** (`esp-tls`, `esp_http_client/server`, `esp_https_ota/server`, `mqtt`, `wifi_provisioning`, `network_provisioning` — all transitive via arduino-esp32) have never been compiled here with TLS disabled. Empirical; containment = drop Step A's line.
- **wpa_supplicant internal-crypto fallback** never built in this project. Empirical; containment = drop Step B's line (the decision rule above already covers the size direction).
- **Runtime coverage:** host tests cannot exercise esp_wifi runtime. Accepted (user decision 2026-08-07): build + host tests only; the next physical flash is the natural smoke test. Residual risk documented, not mitigated.

## Non-goals

- No source changes; no `Crypto.h`/consumer edits.
- No arduino-esp32 component removal or fork (its transitive manifest deps stay).
- No size-reduction target — this is config hygiene, not an optimization phase.
- No on-device verification in this PR.
