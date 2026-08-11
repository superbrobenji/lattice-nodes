# Phase J — Crypto revert: libsodium → mbedtls (higher-level API)

**Date:** 2026-08-06
**Umbrella:** part of `2026-07-22-close-all-open-issues-design.md` (Phase J extension).
**Predecessor:** Phase I (merged to `main` as squash `42df61d`, PR #93).
**Reverts:** Phase I Task 2 (item GG, libsodium swap) — but forward, not via `git revert`: the replacement uses mbedtls's public curve-aware API, not the pre-Phase-I raw-MPI code.

## Goal

Remove libsodium from the firmware and return E2E crypto to mbedtls, recovering the ~22 KB of flash that libsodium links but never uses, without reintroducing the raw-MPI/`MBEDTLS_PRIVATE` footgun the pre-Phase-I code had. Single-purpose PR to `main`.

## Background / evidence

- Phase I Task 2 swapped mbedtls → libsodium predicting −40 to −60 KB; it measured **+82 KB** (task report `.superpowers/sdd/2026-08-06-phaseI-native-idf/task-2-report.md`). Phase I's net −105 KB happened only because Tasks 3 and 6 over-delivered.
- `nm` on the final Phase I ELF: ~22 KB of libsodium primitives linked but never called — `sodium_init()` unconditionally reaches 8 primitive families via `_pick_best_implementation` dispatch (81% of the bloat is `blake2b_compress_ref`). The ESP-IDF libsodium port has no compile-time gating (`SODIUM_LIBRARY_MINIMAL` ignored by its CMakeLists).
- mbedtls stays linked regardless: `esp_wifi` force-selects `MBEDTLS_AES_C` for WPA/PMK. The firmware pays for both crypto libraries.
- `LIBSODIUM_USE_MBEDTLS_SHA=y` (the port's default) already routes libsodium's SHA through mbedtls — further proof both stacks are resident.
- Slimming libsodium via fork = forever-fork maintenance. Revert avoids it.
- The pre-Phase-I mbedtls code was its own hazard: `MBEDTLS_PRIVATE(...)` chains poking `mbedtls_ecdh_context` internals and generic big-endian `mbedtls_mpi_read_binary`/`write_binary` on curve points — brittle across mbedtls versions and the source of the byte-order convention that nearly broke Task 2. This phase uses only public curve-aware APIs.

## Byte-order model (load-bearing)

- **Storage/wire convention is big-endian and immutable.** Device keypairs persisted in NVS, `MasterKeypairFixture.h`, operator-pinned `master_pubkey_pin.h`, and the on-wire `enrollment_public_key`/`secondary_public_key` fields are all BE (legacy of the old raw-MPI export). Changing it = wire/storage break, out of scope.
- **All public curve-aware mbedtls APIs speak RFC 7748 little-endian** for Curve25519 (`mbedtls_ecp_read_key`, `mbedtls_ecp_point_read_binary`, PSA alike).
- Consequence: the BE↔LE conversion cannot vanish — it moves **inside** the wrapper as a private, once-documented detail. `E2ECrypto.h::reverse32()` as a public shim is deleted (decision 5 satisfied in spirit: no shim visible outside the wrapper).
- `mbedtls_ecdh_calc_secret` emits the shared secret in RFC 7748 LE natively for Montgomery curves — empirically verified in Task 2 against real mbedtls 3.6.7, byte-identical to both the old code's output and the current libsodium output. Secret is used verbatim, no conversion.

## Architecture

New file `firmware/main/src/crypto/Crypto.h` — header-only, inline, Tiger-Style, namespace `lattice::crypto`. **Hard boundary: only this file includes mbedtls headers.** `E2ECrypto.h`, `MeshCrypto.h`, `RouteMac.h` keep domain logic (nonce/AAD construction, hop-chain MAC, HKDF labels, key storage convention) and delegate all primitives. A future backend swap touches one file.

```cpp
namespace lattice { namespace crypto {
  // mbedtls_platform_zeroize (replaces sodium_memzero at all call sites)
  void secure_zero(void* buf, size_t len);

  // X25519. BE in/out per storage/wire convention; conversion internal.
  bool x25519_keygen(uint8_t priv32[32], uint8_t pub32[32]);
  bool x25519_shared(const uint8_t priv32[32], const uint8_t peerPub32[32],
                     uint8_t secret32[32]);

  // HKDF-SHA256 (RFC 5869), one-shot. Generalized passthrough signature so the
  // RFC test vectors exercise the wrapper directly; deriveE2EKeys() calls it
  // in the old narrow shape (salt NULL/0, ikm=secret32, one call per label).
  bool hkdf_sha256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen,
                   const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen);

  // HMAC-SHA256, one-shot. Generalized key length for the same reason;
  // RouteMac::chainStep always passes 32.
  bool hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                   uint8_t out32[32]);

  // ChaCha20-Poly1305 (IETF), detached tag, in-place buf. mbedtls_chachapoly_*.
  bool aead_seal(const uint8_t key32[32], const uint8_t nonce12[12],
                 const uint8_t* aad, size_t aadLen,
                 uint8_t* buf, size_t len, uint8_t tag16[16]);
  bool aead_open(const uint8_t key32[32], const uint8_t nonce12[12],
                 const uint8_t* aad, size_t aadLen,
                 uint8_t* buf, size_t len, const uint8_t tag16[16]);
}} // namespace lattice::crypto
```

Wrapper internals:

- **Classic curve-aware API only.** `mbedtls_ecp_gen_keypair` (keygen), `mbedtls_ecp_read_key` (LE private-key load, applies the idempotent X25519 clamp), `mbedtls_ecdh_calc_secret` family for the shared secret. Exact 3.6.x call set (including how the peer public key is loaded without touching private struct fields) is pinned in the implementation plan. **Zero `MBEDTLS_PRIVATE`, zero raw-MPI point I/O.**
- **RNG:** `static int espRng(void*, unsigned char* out, size_t len)` over `esp_fill_random` — hardware TRNG, true-random with RF enabled (WiFi is up before any keygen; same trust posture as the libsodium port's `randombytes_buf`, which sits on `esp_random`). No mbedtls entropy/ctr_drbg modules linked. Host builds get `esp_fill_random` from the test mocks.
- Internal `reverse32` helper, private to the wrapper, documented once with the byte-order model above.

## Error handling

Wrapper returns `bool` (or `void` where the primitive cannot fail meaningfully) and has **no dependency on `lattice::err`**. Callers keep their existing `err::fatal` digits — keygen 1–3 in `MeshCrypto.h`, E2E path 20–27 in `E2ECrypto.h` — preserving the TM1637 error-code map exactly. AEAD failure returns false; callers drop the frame quietly (existing pattern).

## Config + manifest changes

`firmware/sdkconfig.defaults`:

- Re-add: `CONFIG_MBEDTLS_CHACHA20_C=y`, `CONFIG_MBEDTLS_POLY1305_C=y`, `CONFIG_MBEDTLS_CHACHAPOLY_C=y`, `CONFIG_MBEDTLS_HKDF_C=y`, `CONFIG_MBEDTLS_HARDWARE_SHA=y`.
- Keep unchanged: `CONFIG_MBEDTLS_AES_C/GCM_C/CCM_C/HARDWARE_AES/TLS_ENABLED=n` (pre-existing Phase G/A trim) and `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n` (Task 2 follow-up; breaks the esp_wifi → `MBEDTLS_TLS_ENABLED` force-select).
- **Item KK (revived):** disable every `CONFIG_MBEDTLS_ECP_DP_*_ENABLED` except `CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED=y`. Bundle `CONFIG_MBEDTLS_ECDSA_C=n` if the build stays clean (nothing in this firmware verifies certificates; TLS and WPA3-SAE are off). KK was dropped in Phase I as moot-post-libsodium; the revert un-moots it, and ESP-IDF's default otherwise compiles in all NIST/Brainpool curves.
- Rewrite the Task-2 comment block (current lines ~32–50) to describe the reverted state.

`firmware/main/idf_component.yml`: remove `espressif/libsodium` and its comment. `firmware/managed_components/espressif__libsodium/` is untracked; it disappears on the next clean `idf.py reconfigure` — nothing to commit.

## Test changes

- `tests/unit/test_libsodium_kat.cpp` → `tests/unit/test_mbedtls_kat.cpp`. Five KATs, exercised **through the `lattice::crypto` wrapper**, not raw mbedtls: the same three RFC vectors as before (RFC 8439 §2.8.2 ChaCha20-Poly1305, full 114-byte ciphertext + tag; RFC 7748 §5.2 Test 1 X25519; RFC 5869 Appendix A.1 HKDF-SHA256) plus HMAC-SHA256 RFC 4231 Test Case 2 (the wrapper's generalized signature makes it directly testable) and a keygen↔shared round-trip in the BE convention. The RFC 7748 vectors are LE and the wrapper API is BE, so the test feeds byte-reversed vectors — the conversion itself is under test.
- `MasterKeypairFixture.h` e2e scenarios unchanged — a real pre-generated BE keypair; the trap that caught Task 2's byte-order bug remains the compatibility gate for this revert.
- `tests/unit/test_e2e_crypto.cpp` `E2EAead.Rfc8439KnownAnswer`: direct libsodium call → `crypto::aead_seal`.
- `firmware/main/main.cpp`: delete the `sodium_init()` block and `<sodium.h>` include (mbedtls needs no global init). `tests/e2e/harness/NodeContext.cpp`: same.
- `tests/CMakeLists.txt`: replace the libsodium detect/FetchContent block with an mbedtls FetchContent pinned to the 3.6.x line ESP-IDF 5.5.1 bundles (pre-Task-2 block at `a6f8070` is the reference). Link targets `sodium` → `mbedcrypto`.
- Host mocks: add `esp_fill_random` to the existing esp_system mock if absent (`std::random_device`-backed).
- Note: `tests/mocks/libsodium.h` (named in the planning memory) never existed — host tests link real libsodium today and will link real mbedtls after.
- Gate: full suite green, 298/298 (293 pre-existing + 5 wrapper KATs; the 3 libsodium KATs retire).

## Measurement + success criteria

1. Clean `idf.py build` + `idf.py size` before/after, baseline = `main` tip `42df61d`. Build with bounded parallelism (`-j2`; full-parallel OOMs this machine).
2. **Net flash reduction ≥ 15 KB** (expected ~22 KB from dead libsodium, ± mbedtls ECP/chachapoly re-add, KK trim offsetting).
3. 298/298 host tests green; e2e fixture scenarios prove key-compat.
4. Zero functional libsodium remnants, verified three ways: no `sodium` reference in `firmware/main/src/` or `tests/` source; `firmware/main/idf_component.yml` carries no libsodium dependency; `nm` on the final ELF shows zero sodium symbols. Exempt from the letter of the grep: historical/disclaiming comment prose (e.g. `sdkconfig.defaults`' Phase J block explaining *why* libsodium left), and untracked build artifacts (`firmware/sdkconfig`, `dependencies.lock`, `managed_components/`) where `espressif/arduino-esp32`'s own manifest transitively pulls `espressif/libsodium` — proven 0 bytes in the linked image. *(Amended during execution: the original absolute-grep wording was unsatisfiable without deleting legitimate documentation or forking arduino-esp32.)*
5. Single PR `feat/phaseJ-crypto-revert` → `main`, CI green.

## Non-goals

- No change to the BE key storage/wire convention (NVS, fixtures, pin header, hub compat).
- No PSA Crypto API (subsystem flash cost contradicts the phase goal).
- No hub or protocol repo changes; no wire changes.
- No libsodium fork/trim (the road not taken — fork burden).
- Logger, arduino-esp32 dependency, and all other Phase I outcomes untouched.
- No new persisted state, no NVS migration.

## Risks

- **`mbedtls_ecp_read_key` clamp-on-read** for already-clamped legacy keys: idempotent by construction; fixture e2e tests are the proof gate.
- **`calc_secret` LE output** proven empirically on host mbedtls 3.6.7 (Task 2); ESP-IDF 5.5.1 bundles the same 3.6.x line. Residual risk low; KATs catch any deviation.
- **Peer-public-key loading without `MBEDTLS_PRIVATE`** — mbedtls 3.6 public-API surface for building an `mbedtls_ecp_keypair`/ECDH context from raw bytes needs pinning at plan time; if no clean public path exists, the fallback is the TLS-format `mbedtls_ecdh_read_public` (1-byte length prefix + 32 LE bytes), still public API.
- **Kconfig symbol names** for the KK trim verified at build time.
- Host-vs-target mbedtls version skew (FetchContent pin vs ESP-IDF bundle): pin to the same 3.6.x minor; KATs run on both sides.

## Execution notes

- Phase I precondition satisfied 2026-08-06: PR #93 squash-merged to `main` (`42df61d`); umbrella branch deleted local+remote.
- Ruleset `trunk-branches` now carries a repo-admin bypass actor (added 2026-08-06) — escape hatch for the recurring GitHub Actions queue-hang that blocked #93's final checks.
- Scope is small (one wrapper file, three consumer headers, config, tests): expected 3–4 tasks, solo-session executable via subagent-driven development.
