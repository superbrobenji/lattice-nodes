# Phase J Crypto Revert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove libsodium from the firmware and return E2E crypto to mbedtls behind a new `lattice::crypto` wrapper, recovering ~22 KB flash without the raw-MPI/`MBEDTLS_PRIVATE` footgun.

**Architecture:** One new header-only wrapper (`firmware/main/src/crypto/Crypto.h`) is the only file allowed to include mbedtls. The three mesh crypto headers (`E2ECrypto.h`, `MeshCrypto.h`, `RouteMac.h`) keep domain logic and delegate primitives. Storage/wire keys stay big-endian; the wrapper converts BE↔LE privately (all public curve-aware mbedtls APIs are RFC 7748 little-endian).

**Tech Stack:** ESP-IDF 5.5.1 (bundled mbedtls 3.6.x), host tests: GoogleTest + FetchContent mbedtls 3.6.2, CMake, gh CLI.

**Spec:** `docs/superpowers/specs/2026-08-06-phaseJ-crypto-revert-design.md` — read it before starting any task.

## Global Constraints

- Branch: `feat/phaseJ-crypto-revert` (exists, spec committed on it). All commits go here.
- Build parallelism is CAPPED (full-parallel OOMs this machine): host `cmake --build build -j2`, `ctest --parallel 1`, target `idf.py build -- -j2`.
- Pin-file trap: run `rm -f firmware/main/config/master_pubkey_pin.h` before ANY host test run. If that file exists, 26 e2e tests fail spuriously.
- clang-format: CI uses version 18. Always `/opt/homebrew/opt/llvm@18/bin/clang-format` (local Homebrew default is 22 — output differs).
- NEVER use `MBEDTLS_PRIVATE(...)`, never serialize curve points/keys via generic `mbedtls_mpi_read_binary`/`write_binary`. Public curve-aware APIs only.
- The big-endian key storage/wire convention is immutable (NVS keys, `MasterKeypairFixture.h`, `master_pubkey_pin.h`, on-wire fields).
- Host mbedtls pin: 3.6.2 (same block the repo used pre-Phase-I-Task-2; same 3.6.x line ESP-IDF bundles).
- ESP-IDF env: `source $HOME/esp/esp-idf/export.sh`, run `idf.py` from `firmware/`.
- Commits: conventional-commit style, end message with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Host test cycle (from repo root):
  `rm -f firmware/main/config/master_pubkey_pin.h && cd tests && cmake -B build && cmake --build build -j2 && cd build && ctest --parallel 1`

## File Structure

- Create `firmware/main/src/crypto/Crypto.h` — the wrapper. Header-only, inline, Tiger-Style (stack-only, fixed buffers). Sole mbedtls includer.
- Create `tests/mocks/esp_random.h` — host mock for `esp_fill_random` (wrapper's RNG).
- Create `tests/unit/test_mbedtls_kat.cpp` — 5 KATs through the wrapper (AEAD, X25519, HKDF, HMAC, keygen round-trip).
- Modify `firmware/main/src/mesh/E2ECrypto.h`, `MeshCrypto.h`, `RouteMac.h` — delegate to wrapper, drop `<sodium.h>` + `reverse32`.
- Modify `firmware/main/main.cpp`, `tests/e2e/harness/NodeContext.cpp` — drop `sodium_init()`.
- Modify `firmware/main/src/mesh/Enrollment.cpp` — one stale comment.
- Modify `tests/unit/test_e2e_crypto.cpp` — KAT via wrapper, drop `<sodium.h>`.
- Delete `tests/unit/test_libsodium_kat.cpp`.
- Modify `tests/CMakeLists.txt` — mbedtls FetchContent in (Task 1), libsodium block out (Task 2).
- Modify `firmware/sdkconfig.defaults`, `firmware/main/idf_component.yml` — Task 3. (`firmware/main/CMakeLists.txt` already has `mbedtls` in REQUIRES — no change.)

---

### Task 1: `lattice::crypto` wrapper + KATs (host-proven)

**Files:**
- Create: `firmware/main/src/crypto/Crypto.h`
- Create: `tests/mocks/esp_random.h`
- Create: `tests/unit/test_mbedtls_kat.cpp`
- Modify: `tests/CMakeLists.txt` (add mbedtls FetchContent + register test; libsodium stays linked until Task 2)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces (Task 2 relies on these exact signatures, all in `namespace lattice { namespace crypto {`):
  - `void secure_zero(void* buf, size_t len)`
  - `bool x25519_keygen(uint8_t priv32BE[32], uint8_t pub32BE[32])`
  - `bool x25519_shared(const uint8_t priv32BE[32], const uint8_t peerPub32BE[32], uint8_t secret32[32])`
  - `bool hkdf_sha256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen, const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen)`
  - `bool hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out32[32])`
  - `bool aead_seal(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLen, uint8_t* buf, size_t len, uint8_t tag16[16])` — encrypts `buf` in place
  - `bool aead_open(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLen, uint8_t* buf, size_t len, const uint8_t tag16[16])` — decrypts in place

  (Note vs spec sketch: `x25519_keygen` returns `bool` — keygen can fail, spec prose "bool, or void where the primitive cannot fail" governs. `hkdf_sha256`/`hmac_sha256` are generalized passthroughs so the RFC 5869/4231 vectors can exercise the wrapper directly; call sites keep the old narrow shapes.)

- [ ] **Step 1: Verify the RFC 4231 HMAC vector independently** (task-2-report discipline: no hand-trusted hex)

```bash
python3 -c "
import hmac, hashlib
print(hmac.new(b'Jefe', b'what do ya want for nothing?', hashlib.sha256).hexdigest())"
```
Expected: `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843` — must match the `expected[]` array in Step 4's HMAC test byte-for-byte. If it doesn't, fix the test array to the computed value, not vice versa.

- [ ] **Step 2: Add mbedtls FetchContent + mock**

In `tests/CMakeLists.txt`, insert directly ABOVE the `# libsodium — real X25519/...` comment block (~line 17):

```cmake
# mbedtls — real ChaCha20-Poly1305/X25519/HKDF/HMAC for lattice::crypto
# (firmware/main/src/crypto/Crypto.h) on host. Pinned to the same 3.6.x line
# ESP-IDF 5.5.1 bundles (Phase J revert — spec
# docs/superpowers/specs/2026-08-06-phaseJ-crypto-revert-design.md).
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)      # mbedtls' own tests
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
# Newer AppleClang enables -Wunterminated-string-initialization, which mbedtls's
# TLS layer (not needed here — only mbedcrypto is linked) trips on;
# MBEDTLS_FATAL_WARNINGS would promote it to -Werror while compiling it.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(mbedtls)
```

At the bottom, after the existing `add_unit_test(test_libsodium_kat ...)` line, register the new test:

```cmake
add_unit_test(test_mbedtls_kat        unit/test_mbedtls_kat.cpp)
target_link_libraries(test_mbedtls_kat mbedcrypto)
```

Create `tests/mocks/esp_random.h`:

```cpp
// Mock esp_random.h — shadows the ESP-IDF header for host test builds.
// lattice::crypto (firmware/main/src/crypto/Crypto.h) draws all randomness
// through esp_fill_random(); on target that is the hardware TRNG, here a
// seeded mt19937_64 (host tests only — never a crypto-quality source, never
// used for anything persisted).
#pragma once
#include <cstddef>
#include <cstdint>
#include <random>

inline void esp_fill_random(void* buf, size_t len) {
  static std::mt19937_64 rng{std::random_device{}()};
  auto* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) {
    p[i] = static_cast<uint8_t>(rng());
  }
}

inline uint32_t esp_random(void) {
  uint32_t v = 0;
  esp_fill_random(&v, sizeof(v));
  return v;
}
```

- [ ] **Step 3: Write the failing KAT test** — create `tests/unit/test_mbedtls_kat.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "src/crypto/Crypto.h"

// Phase J (crypto revert): known-answer tests for the lattice::crypto wrapper
// (firmware/main/src/crypto/Crypto.h) — the only mbedtls call surface in the
// firmware. These pin the exact byte outputs published in the governing RFCs
// so a broken/misconfigured mbedtls build on either host or ESP-IDF target
// fails loudly here rather than as a silent wire-format break downstream.
// The X25519 test doubles as coverage of the wrapper's private BE<->LE
// conversion: RFC 7748 vectors are little-endian, the wrapper API speaks this
// codebase's big-endian storage/wire convention, so the vectors go in
// byte-reversed and the (LE-native, used-verbatim) secret comes out matching
// the RFC expectation directly.

namespace {
void reverse32(const uint8_t in[32], uint8_t out[32]) {
  for (int i = 0; i < 32; ++i) {
    out[i] = in[31 - i];
  }
}
} // namespace

// RFC 8439 §2.8.2 — ChaCha20-Poly1305 (IETF) AEAD, full 114-byte ciphertext +
// tag + decrypt round trip + tamper rejection, via aead_seal/aead_open.
TEST(MbedtlsKat, ChaCha20Poly1305Rfc8439) {
  const uint8_t key[32] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
                           0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
                           0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
  const uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
                             0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
  const uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
  const char* plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you "
                          "only one tip for the future, sunscreen would be it.";
  ASSERT_EQ(114u, strlen(plaintext));

  const uint8_t expectedCt[114] = {
      0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e,
      0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe, 0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee,
      0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda,
      0x92, 0x72, 0x8b, 0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
      0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c, 0x98, 0x03, 0xae,
      0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85,
      0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc, 0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5,
      0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16};
  const uint8_t expectedTag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
                                   0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};

  uint8_t buf[114];
  memcpy(buf, plaintext, 114);
  uint8_t tag[16];
  ASSERT_TRUE(lattice::crypto::aead_seal(key, nonce, aad, sizeof(aad), buf, 114, tag));
  EXPECT_EQ(0, memcmp(buf, expectedCt, sizeof(buf)));
  EXPECT_EQ(0, memcmp(tag, expectedTag, sizeof(tag)));

  ASSERT_TRUE(lattice::crypto::aead_open(key, nonce, aad, sizeof(aad), buf, 114, tag));
  EXPECT_EQ(0, memcmp(buf, plaintext, 114));

  // Tamper rejection: flip one ciphertext bit, open must fail.
  ASSERT_TRUE(lattice::crypto::aead_seal(key, nonce, aad, sizeof(aad), buf, 114, tag));
  buf[0] ^= 0x01;
  EXPECT_FALSE(lattice::crypto::aead_open(key, nonce, aad, sizeof(aad), buf, 114, tag));
}

// RFC 7748 §5.2, X25519 Test 1. RFC bytes are LE; wrapper takes BE — reversal
// at the test boundary IS the conversion under test. Output secret is
// LE-native and used verbatim, so it compares against the RFC bytes directly.
TEST(MbedtlsKat, X25519Rfc7748Test1) {
  const uint8_t scalarLE[32] = {0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15,
                                0x4b, 0x82, 0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc,
                                0x5a, 0x18, 0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
  const uint8_t uLE[32] = {0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1,
                           0xa4, 0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3,
                           0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
  const uint8_t expected[32] = {0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea,
                                0x4d, 0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c,
                                0x71, 0xf7, 0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};

  uint8_t scalarBE[32], uBE[32], out[32];
  reverse32(scalarLE, scalarBE);
  reverse32(uLE, uBE);
  ASSERT_TRUE(lattice::crypto::x25519_shared(scalarBE, uBE, out));
  EXPECT_EQ(0, memcmp(out, expected, sizeof(expected)));
}

// RFC 5869 Appendix A.1, Test Case 1 — HKDF-SHA256 through the wrapper's
// generalized signature (deriveE2EKeys() uses the narrow salt=NULL/0 shape).
TEST(MbedtlsKat, HkdfSha256Rfc5869TestCase1) {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  const uint8_t salt[13] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                            0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
  const uint8_t info[10] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};

  const uint8_t expectedOkm[42] = {0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f,
                                   0x64, 0xd0, 0x36, 0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a,
                                   0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf, 0x34,
                                   0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65};

  uint8_t okm[42];
  ASSERT_TRUE(lattice::crypto::hkdf_sha256(ikm, sizeof(ikm), salt, sizeof(salt), info,
                                           sizeof(info), okm, sizeof(okm)));
  EXPECT_EQ(0, memcmp(okm, expectedOkm, sizeof(expectedOkm)));
}

// RFC 4231 Test Case 2 — HMAC-SHA256 ("Jefe"). Expected bytes independently
// re-derived via Python stdlib hmac before being trusted here (see plan).
TEST(MbedtlsKat, HmacSha256Rfc4231TestCase2) {
  const uint8_t key[4] = {'J', 'e', 'f', 'e'};
  const char* data = "what do ya want for nothing?";
  const uint8_t expected[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
                                0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                                0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
                                0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
  uint8_t out[32];
  ASSERT_TRUE(lattice::crypto::hmac_sha256(key, sizeof(key),
                                           reinterpret_cast<const uint8_t*>(data), strlen(data),
                                           out));
  EXPECT_EQ(0, memcmp(out, expected, sizeof(expected)));
}

// Keygen + ECDH self-consistency in the BE convention: two fresh keypairs,
// both shared-secret directions agree and are nonzero. Validates
// x25519_keygen's export path against x25519_shared's import path before any
// consumer (Task 2) depends on them.
TEST(MbedtlsKat, KeygenSharedRoundTrip) {
  uint8_t aPriv[32], aPub[32], bPriv[32], bPub[32], s1[32], s2[32];
  ASSERT_TRUE(lattice::crypto::x25519_keygen(aPriv, aPub));
  ASSERT_TRUE(lattice::crypto::x25519_keygen(bPriv, bPub));
  ASSERT_TRUE(lattice::crypto::x25519_shared(aPriv, bPub, s1));
  ASSERT_TRUE(lattice::crypto::x25519_shared(bPriv, aPub, s2));
  EXPECT_EQ(0, memcmp(s1, s2, sizeof(s1)));
  const uint8_t zero[32] = {0};
  EXPECT_NE(0, memcmp(s1, zero, sizeof(zero)));
}
```

- [ ] **Step 4: Run the new test to verify it fails**

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && cmake -B build && cmake --build build -j2 --target test_mbedtls_kat
```
Expected: FAIL to compile — `src/crypto/Crypto.h: No such file or directory`.

- [ ] **Step 5: Implement the wrapper** — create `firmware/main/src/crypto/Crypto.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <esp_random.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

// lattice::crypto — the firmware's single crypto-primitive surface (Phase J).
// This is the ONLY file that includes mbedtls headers; E2ECrypto.h,
// MeshCrypto.h and RouteMac.h delegate here. Swap the backend again and only
// this file changes.
//
// Byte-order model (load-bearing — read before touching X25519 code):
// This codebase stores and transmits Curve25519 keys BIG-ENDIAN — a legacy of
// the pre-Phase-I raw-MPI export (mbedtls_mpi_write_binary is BE). Every
// persisted device keypair (NVS), the e2e MasterKeypairFixture.h, operator
// master_pubkey_pin.h headers, and the on-wire enrollment_public_key /
// secondary_public_key fields all use that convention; it cannot change
// without a wire/storage break. mbedtls's public curve-aware APIs
// (mbedtls_ecp_read_key, mbedtls_ecdh_read_public) speak RFC 7748
// LITTLE-ENDIAN, so keys are byte-reversed at this boundary — and nowhere
// else. The shared secret needs NO reversal: mbedtls_ecdh_calc_secret emits
// RFC 7748 LE natively for Montgomery curves (verified empirically against
// mbedtls 3.6 during Phase I Task 2 — see that task's report), which is
// byte-identical to what both the old raw-MPI code and the Phase I libsodium
// code produced. Consumers therefore see: BE keys in/out, secret verbatim.
//
// No MBEDTLS_PRIVATE anywhere, no generic-MPI point serialization — the two
// footguns of the pre-Phase-I implementation. Public API calls only.
// Tiger-Style: stack-only, fixed-size buffers, contexts freed on every path.

namespace lattice {
namespace crypto {

namespace detail {

// f_rng adapter over the hardware TRNG (true-random with RF enabled; WiFi is
// up before any keygen — Mesh::init runs before Enrollment::init). Host test
// builds get esp_fill_random from tests/mocks/esp_random.h.
inline int espRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

inline void reverse32(const uint8_t in[32], uint8_t out[32]) {
  for (int i = 0; i < 32; ++i) {
    out[i] = in[31 - i];
  }
}

} // namespace detail

inline void secure_zero(void* buf, size_t len) {
  mbedtls_platform_zeroize(buf, len);
}

// Fresh X25519 keypair, exported in the BE storage/wire convention.
// mbedtls_ecp_gen_keypair produces a clamped private scalar (RFC 7748), same
// as the old keygen — stored keys remain pre-clamped.
inline bool x25519_keygen(uint8_t priv32BE[32], uint8_t pub32BE[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point Q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&Q);

  uint8_t privLE[32], pubLE[32];
  size_t olen = 0;
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
            mbedtls_ecp_gen_keypair(&grp, &d, &Q, detail::espRng, nullptr) == 0 &&
            mbedtls_mpi_write_binary_le(&d, privLE, 32) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pubLE,
                                           32) == 0 &&
            olen == 32;
  if (ok) {
    detail::reverse32(privLE, priv32BE);
    detail::reverse32(pubLE, pub32BE);
  }
  mbedtls_platform_zeroize(privLE, sizeof(privLE));
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// X25519 ECDH: BE key inputs, secret output verbatim (LE-native — see header
// comment). mbedtls_ecp_read_key applies the RFC 7748 clamp masks on load
// (idempotent for the pre-clamped keys this codebase stores).
// mbedtls_ecdh_read_public takes the TLS ECPoint wire form: for Montgomery
// curves that is a 1-byte length prefix (32) + the 32-byte LE u-coordinate.
inline bool x25519_shared(const uint8_t priv32BE[32], const uint8_t peerPub32BE[32],
                          uint8_t secret32[32]) {
  uint8_t privLE[32];
  uint8_t peerTls[33];
  detail::reverse32(priv32BE, privLE);
  peerTls[0] = 32;
  detail::reverse32(peerPub32BE, peerTls + 1);

  mbedtls_ecdh_context ctx;
  mbedtls_ecp_keypair kp;
  mbedtls_ecdh_init(&ctx);
  mbedtls_ecp_keypair_init(&kp);

  size_t olen = 0;
  bool ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &kp, privLE, 32) == 0 &&
            mbedtls_ecdh_get_params(&ctx, &kp, MBEDTLS_ECDH_OURS) == 0 &&
            mbedtls_ecdh_read_public(&ctx, peerTls, sizeof(peerTls)) == 0 &&
            mbedtls_ecdh_calc_secret(&ctx, &olen, secret32, 32, detail::espRng, nullptr) == 0 &&
            olen == 32;
  mbedtls_platform_zeroize(privLE, sizeof(privLE));
  mbedtls_ecp_keypair_free(&kp);
  mbedtls_ecdh_free(&ctx);
  return ok;
}

// HKDF-SHA256 (RFC 5869), one-shot Extract-then-Expand.
inline bool hkdf_sha256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen,
                        const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen) {
  return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, saltLen, ikm, ikmLen,
                      info, infoLen, out, outLen) == 0;
}

// HMAC-SHA256, one-shot.
inline bool hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                        uint8_t out32[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, data, len,
                         out32) == 0;
}

// ChaCha20-Poly1305 (IETF), detached tag, in-place buf (mbedtls documents
// in-place src==dst as supported for chachapoly).
inline bool aead_seal(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, uint8_t tag16[16]) {
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  bool ok = mbedtls_chachapoly_setkey(&ctx, key32) == 0 &&
            mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce12, aad, aadLen, buf, buf,
                                               tag16) == 0;
  mbedtls_chachapoly_free(&ctx);
  return ok;
}

inline bool aead_open(const uint8_t key32[32], const uint8_t nonce12[12], const uint8_t* aad,
                      size_t aadLen, uint8_t* buf, size_t len, const uint8_t tag16[16]) {
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  bool ok = mbedtls_chachapoly_setkey(&ctx, key32) == 0 &&
            mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce12, aad, aadLen, tag16, buf, buf) == 0;
  mbedtls_chachapoly_free(&ctx);
  return ok;
}

} // namespace crypto
} // namespace lattice
```

- [ ] **Step 6: Run the KATs to verify they pass**

```bash
cd tests && cmake --build build -j2 --target test_mbedtls_kat && ./build/test_mbedtls_kat
```
Expected: 5/5 PASS. If `X25519Rfc7748Test1` fails while others pass, the `ecdh_get_params`/`read_public` route misbehaved — fall back per spec risk note: replace the peer-load with `mbedtls_ecdh_setup(&ctx, MBEDTLS_ECP_DP_CURVE25519)` before `get_params`, and if still failing, escalate to orchestrator with the exact mbedtls error code (do NOT reach for `MBEDTLS_PRIVATE`).

- [ ] **Step 7: Full suite still green** (sodium still linked — nothing existing should move)

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && cmake --build build -j2 && cd build && ctest --parallel 1
```
Expected: 301 tests pass (296 pre-existing + 5 new). Zero failures.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/src/crypto/Crypto.h tests/mocks/esp_random.h \
        tests/unit/test_mbedtls_kat.cpp tests/CMakeLists.txt
git commit -m "feat(phaseJ): lattice::crypto wrapper over mbedtls + RFC KATs

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Migrate consumers, remove libsodium from host build

**Files:**
- Modify: `firmware/main/src/mesh/E2ECrypto.h` (full rewrite below)
- Modify: `firmware/main/src/mesh/MeshCrypto.h` (full rewrite below)
- Modify: `firmware/main/src/mesh/RouteMac.h` (chainStep + includes)
- Modify: `firmware/main/main.cpp` (~lines 23, 403-411), `tests/e2e/harness/NodeContext.cpp` (~lines 4, 54-62), `firmware/main/src/mesh/Enrollment.cpp` (~lines 38-41 comment)
- Modify: `tests/unit/test_e2e_crypto.cpp` (KAT body + include)
- Delete: `tests/unit/test_libsodium_kat.cpp`
- Modify: `tests/CMakeLists.txt` (libsodium block out, `mbedcrypto` everywhere)

**Interfaces:**
- Consumes: every `lattice::crypto::*` signature from Task 1 (listed there).
- Produces: unchanged public surfaces — `lattice::mesh::crypto::{computeSharedSecret, deriveE2EKeys, sealPayload, openPayload, buildNonce, buildAad, generateKeypair, registerPeerWithEspNow}` and `lattice::mesh::routemac::{buildHopContext, chainStep}` keep their exact existing signatures (Mesh.cpp/Enrollment.cpp/PeerRegistry.cpp call them; they must not need edits beyond the Enrollment comment).

- [ ] **Step 1: Rewrite `firmware/main/src/mesh/E2ECrypto.h`** — replace the entire file:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "src/crypto/Crypto.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {
namespace crypto {

// X25519 ECDH shared secret (Phase J: back on mbedtls via lattice::crypto —
// see src/crypto/Crypto.h for the byte-order model). Keys arrive in this
// codebase's big-endian storage/wire convention; the returned secret is
// byte-identical to what the pre-Phase-I mbedtls code and the Phase I
// libsodium code both produced. err::fatal digits 20-25 (LMK path used 10-19).
inline void computeSharedSecret(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                                uint8_t* secret32Out) {
  if (!lattice::crypto::x25519_shared(ownPrivateKey32, peerPublicKey32, secret32Out)) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 25,
                        "MESH: computeSharedSecret — x25519 failed");
  }
}

// Direction-split E2E keys (spec §2): HKDF-SHA256 over the ECDH secret.
// k_up seals node→master payloads, k_down master→node. Salt NULL/0, one
// one-shot HKDF per label — byte-identical to the original mbedtls_hkdf
// shape (and to the Phase I extract-once/expand-twice equivalent).
inline void deriveE2EKeys(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                          uint8_t* kUp32Out, uint8_t* kDown32Out) {
  uint8_t secret[32];
  computeSharedSecret(ownPrivateKey32, peerPublicKey32, secret);

  static const uint8_t upLabel[] = "lattice-e2e-up-v3";
  static const uint8_t downLabel[] = "lattice-e2e-down-v3";

  bool ok = lattice::crypto::hkdf_sha256(secret, sizeof(secret), nullptr, 0, upLabel,
                                         sizeof(upLabel) - 1, kUp32Out, 32) &&
            lattice::crypto::hkdf_sha256(secret, sizeof(secret), nullptr, 0, downLabel,
                                         sizeof(downLabel) - 1, kDown32Out, 32);
  lattice::crypto::secure_zero(secret, sizeof(secret));
  if (!ok) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 26,
                        "MESH: deriveE2EKeys — hkdf failed");
  }
}

// AEAD framing (spec §1/§2).
// Nonce (12B): epoch(4 LE) || seq(2 LE) || origin_mac(6) — unique per key given the
// boot-epoch counter and the seq-wrap epoch bump.
// AAD (24B): version, type, data_type, origin, target, epoch, seq — immutable fields only.
constexpr size_t E2E_AAD_LEN = 24;
constexpr size_t E2E_NONCE_LEN = 12;

// Phase I Task 7 (SS): byte-by-byte little-endian shift+mask+store replaced
// with memcpy. ESP32 (Xtensa and RISC-V variants alike) is little-endian, so
// a direct memcpy of these fixed-width fields reproduces the prior output
// bit-for-bit — verified by the existing buildNonce/buildAad test coverage
// (tests/unit/test_mesh_logic.cpp et al.), which pins the exact wire bytes.
// memcpy (not a raw pointer cast/dereference) is required here, not just
// cosmetic: mesh_message is __attribute__((packed)), so msg.epoch_num /
// msg.seq_num are not guaranteed 4-/2-byte aligned, and Xtensa faults on
// unaligned word loads through a typed pointer — memcpy is well-defined for
// any alignment.
inline void buildNonce(const mesh_message& msg, uint8_t nonce[E2E_NONCE_LEN]) {
  memcpy(nonce + 0, &msg.epoch_num, sizeof(msg.epoch_num));
  memcpy(nonce + 4, &msg.seq_num, sizeof(msg.seq_num));
  memcpy(nonce + 6, msg.origin_mac_address, 6);
}

inline void buildAad(const mesh_message& msg, uint8_t aad[E2E_AAD_LEN]) {
  aad[0] = msg.proto_version;
  aad[1] = msg.message_type;
  // data_type is int32_t; memcpy copies its 4-byte little-endian bit pattern
  // directly — identical to the prior uint32_t-cast shift+mask+store (a
  // static_cast<uint32_t> from int32_t never changes the underlying bits).
  memcpy(aad + 2, &msg.data_type, sizeof(msg.data_type));
  memcpy(aad + 6, msg.origin_mac_address, 6);
  memcpy(aad + 12, msg.target_mac_address, 6);
  memcpy(aad + 18, &msg.epoch_num, sizeof(msg.epoch_num));
  memcpy(aad + 22, &msg.seq_num, sizeof(msg.seq_num));
}

// Encrypts msg.data in place and writes msg.auth_tag. Returns false on
// backend error. msg.data and msg.auth_tag are not adjacent in mesh_message
// (route_len/route_path sit between them) — the wrapper's detached-tag shape
// writes them independently.
inline bool sealPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  return lattice::crypto::aead_seal(key32, nonce, aad, E2E_AAD_LEN, msg.data, sizeof(msg.data),
                                    msg.auth_tag);
}

// Decrypts msg.data in place, verifying msg.auth_tag. Returns false on tag
// mismatch or backend error — callers drop the frame quietly (finding-#9
// pattern).
inline bool openPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  return lattice::crypto::aead_open(key32, nonce, aad, E2E_AAD_LEN, msg.data, sizeof(msg.data),
                                    msg.auth_tag);
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
```

- [ ] **Step 2: Rewrite `firmware/main/src/mesh/MeshCrypto.h`** — replace the entire file:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <esp_now.h>
#include "src/crypto/Crypto.h"
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"

namespace lattice {
namespace mesh {
namespace crypto {

// Register an ESP-NOW peer WITHOUT link-layer encryption (spec §2, proto v3):
// payload confidentiality/integrity is end-to-end (E2ECrypto.h), and unencrypted
// slots raise the ESP-NOW peer cap from ~6 to 20. The shared PMK stays set.
inline void registerPeerWithEspNow(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac))
    return;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = false;
  lattice::err::checkEsp(esp_now_add_peer(&info), lattice::utils::ErrorType::COMMUNICATION_FAIL,
                         "registerPeerWithEspNow: add_peer failed");
}

// Extract ONLY the key generation branch from Mesh::loadOrGenerateKeypair().
// The load-from-EEPROM branch and EEPROM save remain in loadOrGenerateKeypair().
//
// Phase J: keygen via lattice::crypto::x25519_keygen (mbedtls, public
// curve-aware API — see src/crypto/Crypto.h for the byte-order model). Keys
// come back in the established big-endian storage/wire convention, already
// clamped, so on-device keys generated by any prior firmware generation —
// and this device's own future reloads of what it just generated — keep
// interpreting the same bytes the same way.
// err::fatal digit 3 (digits 1-2 belonged to the old entropy-seed and
// group-load sub-steps, now internal to the wrapper — retired).
inline void generateKeypair(uint8_t* priv32Out, uint8_t* pub32Out) {
  if (!lattice::crypto::x25519_keygen(priv32Out, pub32Out)) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 3,
                        "MESH: keypair gen failed");
  }
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
```

- [ ] **Step 3: Update `firmware/main/src/mesh/RouteMac.h`** — three edits:
  1. Replace `#include <sodium.h>` with `#include "src/crypto/Crypto.h"`.
  2. Replace the `chainStep` comment's last paragraph (`// Phase I Task 2: libsodium — was mbedtls_md_hmac...` through `...no state-variant needed.`) with:

```cpp
// Phase J: HMAC via lattice::crypto::hmac_sha256 (mbedtls one-shot).
```

  3. Replace the `chainStep` body's HMAC call:

```cpp
  uint8_t full[32]; // SHA-256 output
  if (!lattice::crypto::hmac_sha256(secret, 32, input, sizeof(input), full)) {
    // Fail-safe: an unwritable MAC must not leak garbage — zero it so the
    // frame fails verification at the master and gets dropped.
    memset(out_mac, 0, AUTH_PATH_LEN);
    return;
  }
  memcpy(out_mac, full, AUTH_PATH_LEN); // truncate to first 8 bytes
```

- [ ] **Step 4: Drop `sodium_init()` from boot paths**
  - `firmware/main/main.cpp`: delete `#include <sodium.h>` (line ~23) and the whole `// Phase I Task 2: libsodium — must run before any crypto:: call...` comment + `if (sodium_init() < 0) {...}` block (lines ~403-411). mbedtls needs no global init; nothing replaces it.
  - `tests/e2e/harness/NodeContext.cpp`: delete `#include <sodium.h>` (line 4) and the comment + `if (sodium_init() < 0) {...}` block in the constructor (lines ~54-62). Keep `eepromData.fill(0xFF);` and everything after.
  - `firmware/main/src/mesh/Enrollment.cpp` (lines ~38-41): replace the comment's second sentence so it reads:

```cpp
// NOTE: Enrollment::init() and Enrollment::enrollPeer() are crypto-heavy (X25519
// keygen via MeshCrypto.h::generateKeypair). Host test builds compile them for real
// against a host-built mbedtls (Phase J revert; see tests/CMakeLists.txt).
```

- [ ] **Step 5: Migrate `tests/unit/test_e2e_crypto.cpp`**
  - Delete `#include <sodium.h>` (line 6).
  - In `TEST(E2EAead, Rfc8439KnownAnswer)`: update the lead comment to say `exercised against the same wrapper call sealPayload uses (lattice::crypto::aead_seal; Phase J — spec §Test changes)`, and replace the body after `expectedTag` with:

```cpp
  uint8_t buf[114], tag[16];
  memcpy(buf, plaintext, 114);
  ASSERT_TRUE(lattice::crypto::aead_seal(key, nonce, aad, sizeof(aad), buf, 114, tag));
  EXPECT_EQ(0, memcmp(tag, expectedTag, 16));
  EXPECT_EQ(0xd3, buf[0]); // first ciphertext byte per RFC 8439 §2.8.2
```

  (Add `#include "src/crypto/Crypto.h"` next to the file's other includes if not already transitively present — it is via E2ECrypto.h, but the direct call warrants the direct include.)

- [ ] **Step 6: Delete the libsodium KAT + purge libsodium from `tests/CMakeLists.txt`**

```bash
git rm tests/unit/test_libsodium_kat.cpp
```

In `tests/CMakeLists.txt`:
  1. Delete the entire libsodium block — from the `# libsodium — real X25519/ChaCha20-Poly1305/HKDF-SHA256/HMAC-SHA256 for` comment down through the `endif()` that closes the FetchContent fallback (the block ending with `add_library(sodium INTERFACE)` / `target_link_libraries(sodium INTERFACE ...libsodium.a)`).
  2. In the `add_unit_test` macro: `target_link_libraries(${name} gtest_main gmock sodium)` → `target_link_libraries(${name} gtest_main gmock mbedcrypto)`.
  3. Delete the line `add_unit_test(test_libsodium_kat      unit/test_libsodium_kat.cpp)`.
  4. Delete the now-redundant `target_link_libraries(test_mbedtls_kat mbedcrypto)` (macro covers it).
  5. `target_link_libraries(lattice_e2e gtest_main gmock sodium)` → `... mbedcrypto`.

- [ ] **Step 7: Full clean host suite**

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && rm -rf build && cmake -B build && cmake --build build -j2 && cd build && ctest --parallel 1
```
Expected: 298 tests, all pass (301 from Task 1 minus the 3 deleted libsodium KATs). The `lattice_e2e` MasterKeypairFixture scenarios are the byte-order compatibility gate — if ANY of them fail, the wrapper's BE↔LE handling is wrong; stop and debug the wrapper, do not touch the fixtures.

- [ ] **Step 8: Verify zero sodium references**

```bash
grep -rin "sodium" firmware/ tests/ --exclude-dir=build --exclude-dir=managed_components
```
Expected: no output. (Catches stray comments too — spec success criterion 4.)

- [ ] **Step 9: Commit**

```bash
git add -A firmware/main tests
git commit -m "feat(phaseJ): migrate E2E crypto consumers to lattice::crypto, drop libsodium from host build

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: sdkconfig revert + KK curve trim + manifest; target build + size gate

**Files:**
- Modify: `firmware/sdkconfig.defaults` (lines ~32-50 comment+block; append KK block after line 87)
- Modify: `firmware/main/idf_component.yml`

**Interfaces:**
- Consumes: Task 2's migrated firmware sources (target build compiles them against mbedtls for the first time here — the target build is EXPECTED broken between Task 2 and this task's sdkconfig re-enable; host suite was the Task 2 gate).
- Produces: a clean `idf.py build` + measured size for Task 4's PR body.

- [ ] **Step 1: Rewrite the mbedtls block in `firmware/sdkconfig.defaults`** — replace the comment block + five `=n` lines (currently lines ~32-50, from `# Phase I Task 2 (item GG): mbedtls fully dropped...` through `CONFIG_MBEDTLS_TLS_ENABLED=n`) with:

```
# Phase J: E2E crypto back on mbedtls — libsodium reverted (measured +82KB in
# Phase I Task 2 vs the predicted win; ~22KB of it never-called primitives
# reachable only via sodium_init()'s dispatch tables — see
# docs/superpowers/specs/2026-08-06-phaseJ-crypto-revert-design.md).
# E2ECrypto.h/MeshCrypto.h/RouteMac.h now call mbedtls exclusively through
# lattice::crypto (firmware/main/src/crypto/Crypto.h). CHACHA20/POLY1305/
# CHACHAPOLY seal the E2E payloads, HKDF derives the direction-split keys,
# HARDWARE_SHA accelerates HMAC/HKDF (SHA-256 hardware block).
CONFIG_MBEDTLS_CHACHA20_C=y
CONFIG_MBEDTLS_POLY1305_C=y
CONFIG_MBEDTLS_CHACHAPOLY_C=y
CONFIG_MBEDTLS_HKDF_C=y
CONFIG_MBEDTLS_HARDWARE_SHA=y
# AES/GCM/CCM/TLS below are a SEPARATE, pre-existing Phase G/Phase A size trim
# (no TLS/HTTPS use at all, independent of which library E2E crypto calls).
# esp_wifi's Enterprise force-select chain on MBEDTLS_TLS_ENABLED is broken by
# CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n further down.
CONFIG_MBEDTLS_AES_C=n
CONFIG_MBEDTLS_GCM_C=n
CONFIG_MBEDTLS_CCM_C=n
CONFIG_MBEDTLS_HARDWARE_AES=n
CONFIG_MBEDTLS_TLS_ENABLED=n
```

Then update the stale sentence in the `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT` comment (lines ~81-86): delete its last sentence (`LIBSODIUM_USE_MBEDTLS_SHA=y kept at default — measured: ... esp_wifi's WPA/PMK path).`) — libsodium is gone; keep the rest of that comment as-is.

- [ ] **Step 2: Append the KK curve-trim block** at the end of `sdkconfig.defaults`:

```
# Phase J item KK (revived): only Curve25519 is used (X25519 E2E keys).
# ESP-IDF's default enables every NIST/Brainpool curve in mbedtls — dead
# flash here: TLS off, WPA3-SAE off, WiFi Enterprise off, no cert use.
CONFIG_MBEDTLS_ECP_DP_SECP192R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP224R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP521R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP192K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP224K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_SECP256K1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP256R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP384R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_BP512R1_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_CURVE448_ENABLED=n
CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED=y
# Nothing signs or verifies with ECDSA (no certs). Drop this single line if
# the build reports an unmet Kconfig dependency on it.
CONFIG_MBEDTLS_ECDSA_C=n
```

- [ ] **Step 3: Remove libsodium from `firmware/main/idf_component.yml`** — the file becomes exactly:

```yaml
## IDF Component Manager Manifest File
dependencies:
  idf:
    version: ">=5.0.0"
  espressif/arduino-esp32:
    version: "^3.3.10"
```

- [ ] **Step 4: Clean target build**

```bash
cd firmware
rm -rf build sdkconfig managed_components dependencies.lock
source $HOME/esp/esp-idf/export.sh
idf.py reconfigure && idf.py build -- -j2
```
Expected: clean build. Verify libsodium is gone and the Kconfig lines took:

```bash
ls managed_components/                              # NO espressif__libsodium
grep -c "MBEDTLS_ECP_DP_.*=y" build/../sdkconfig    # exactly 1 (CURVE25519)
grep "MBEDTLS_CHACHAPOLY_C\|MBEDTLS_ECDSA_C" sdkconfig
```
Expected: `CONFIG_MBEDTLS_CHACHAPOLY_C=y`, `CONFIG_MBEDTLS_ECDSA_C` unset or `=n`. If the build fails on `CONFIG_MBEDTLS_ECDSA_C=n` (unmet dependency), remove that one line and rebuild — note it in the task report.

- [ ] **Step 5: Measure size**

```bash
idf.py size
```
Reference baseline: **777,635 B total image** at Phase I completion (`.superpowers/sdd/2026-08-06-phaseI-native-idf/task-10-report.md`, final column; the two post-Task-10 cleanup commits are size-neutral to within noise). Gate: **new total ≤ 762,275 B** (≥ 15 KB recovered; ~22 KB expected). If the number looks implausible (e.g. larger than baseline), re-baseline by building `main` tip in a scratch worktree:

```bash
git worktree add /tmp/phaseJ-baseline 42df61d
cd /tmp/phaseJ-baseline/firmware && rm -rf build sdkconfig managed_components dependencies.lock
idf.py reconfigure && idf.py build -- -j2 && idf.py size
cd /Users/benji/projects/personal/lattice-nodes && git worktree remove --force /tmp/phaseJ-baseline
```
Record both numbers (baseline, new, delta) — Task 4's PR body needs them.

- [ ] **Step 6: Commit**

```bash
git add firmware/sdkconfig.defaults firmware/main/idf_component.yml
git commit -m "feat(phaseJ): re-enable mbedtls chachapoly/hkdf, drop libsodium component, KK curve trim

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Final sweep + PR

**Files:** none new — verification + `git push` + `gh pr create`.

**Interfaces:**
- Consumes: Task 3's size numbers (baseline / new / delta) for the PR body.
- Produces: open PR `feat/phaseJ-crypto-revert` → `main`.

- [ ] **Step 1: Format check with CI's clang-format (v18, NOT local 22)**

```bash
/opt/homebrew/opt/llvm@18/bin/clang-format -i \
  firmware/main/src/crypto/Crypto.h \
  firmware/main/src/mesh/E2ECrypto.h \
  firmware/main/src/mesh/MeshCrypto.h \
  firmware/main/src/mesh/RouteMac.h \
  tests/mocks/esp_random.h \
  tests/unit/test_mbedtls_kat.cpp
git diff --stat
```
If diffs appear, inspect (`git diff`), then commit them as `style(phaseJ): clang-format 18 pass`. Do NOT format `main.cpp`, `NodeContext.cpp`, `Enrollment.cpp`, `test_e2e_crypto.cpp` whole-file — their edits were surgical; hand-match surrounding style instead (Task 2 already did).

- [ ] **Step 2: Full verification sweep** (run all four, in order)

```bash
rm -f firmware/main/config/master_pubkey_pin.h
cd tests && rm -rf build && cmake -B build && cmake --build build -j2 && cd build && ctest --parallel 1
```
Expected: 298/298 pass.

```bash
grep -rin "sodium" firmware/ tests/ --exclude-dir=build --exclude-dir=managed_components
```
Expected: empty.

```bash
grep -rn "MBEDTLS_PRIVATE\|mbedtls_mpi_read_binary\b\|mbedtls_mpi_write_binary\b" firmware/main/src/
```
Expected: empty (the `_le` variant in Crypto.h is fine and won't match the `\b`-anchored patterns).

```bash
cd firmware && idf.py build -- -j2
```
Expected: clean (incremental — full clean build already done in Task 3).

- [ ] **Step 3: Push + PR**

```bash
git push -u origin feat/phaseJ-crypto-revert
gh pr create --base main --title "Phase J: crypto revert — libsodium -> mbedtls via lattice::crypto wrapper" --body "$(cat <<'EOF'
Reverts Phase I Task 2 (item GG) forward: E2E crypto returns to mbedtls behind a new single-file wrapper (`firmware/main/src/crypto/Crypto.h`), using only public curve-aware APIs — no `MBEDTLS_PRIVATE`, no raw-MPI point serialization, no byte-order shim outside the wrapper.

Spec: `docs/superpowers/specs/2026-08-06-phaseJ-crypto-revert-design.md`

## Why
- Task 2 measured **+82 KB** (predicted −40 to −60 KB); ~22 KB of libsodium is linked but never called (`sodium_init()` dispatch reachability).
- mbedtls never left (esp_wifi WPA/PMK) — we paid for both libraries.

## What
- New `lattice::crypto` wrapper: X25519 keygen/ECDH, HKDF-SHA256, HMAC-SHA256, ChaCha20-Poly1305 detached AEAD, secure_zero. BE storage/wire key convention preserved byte-for-byte (MasterKeypairFixture e2e scenarios gate this).
- 5 RFC KATs through the wrapper (RFC 8439 / 7748 / 5869 / 4231 + keygen round-trip).
- sdkconfig: chachapoly/hkdf/hardware-SHA re-enabled; item KK revived (Curve25519-only ECP).
- libsodium removed from `idf_component.yml` + host test build.

## Size
| | Total image |
|---|---|
| Baseline (`42df61d`) | <BASELINE> B |
| Phase J | <NEW> B |
| Delta | **<DELTA> B** |

## Tests
298/298 host tests green (293 pre-existing + 5 wrapper KATs; 3 libsodium KATs retired).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
Fill `<BASELINE>`/`<NEW>`/`<DELTA>` with Task 3's measured numbers before running.

- [ ] **Step 4: Watch CI**

```bash
gh pr checks --watch
```
Expected: all 7 required contexts green. Known issue: GitHub Actions queue-hangs (e2e / `Analyze (cpp)` stuck `queued`) — if checks hang > ~30 min with the rest green, report to the user; a repo-admin bypass exists on the ruleset (`gh pr merge --admin`), merge is the user's call.

---

## Verification Summary (spec success criteria → plan coverage)

1. Net flash ≥ 15 KB — Task 3 Step 5 gate.
2. 298/298 host tests + fixture e2e compat — Task 2 Step 7, Task 4 Step 2.
3. Zero `sodium` references in `firmware/` + `tests/` — Task 2 Step 8, Task 4 Step 2.
4. No `MBEDTLS_PRIVATE` / raw-MPI — Task 4 Step 2 grep.
5. Single PR to `main`, CI green — Task 4 Steps 3-4.
