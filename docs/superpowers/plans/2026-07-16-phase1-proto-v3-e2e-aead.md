<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Phase 1: Protocol v3 + E2E AEAD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Protocol v3 wire format with end-to-end ChaCha20-Poly1305 payload sealing between enrolled nodes and their master (direct range only), replacing per-peer ESP-NOW LMK encryption.

**Architecture:** Wire format changes land in the `../lattice-protocol` repo (source of truth: `message/message.go` → codegen → `c/mesh_message.h`), then the submodule is bumped here. New header-only `E2ECrypto.h` derives direction-split keys (HKDF-SHA256 over the existing Curve25519 ECDH secret) and seals/opens `msg.data[64]` in place with the AEAD tag in a new `auth_tag[16]` field. A small `E2EKeyStore` caches derived keys per peer. Leaf nodes seal self-originated uplink (`ADAPTER_DATA`, `ROUTE_REPORT`); the master opens on local delivery. Relays never touch the payload.

**Tech Stack:** C++ (ESP32 firmware, host-tested), mbedtls 3.6.2 (`chachapoly.h`, `hkdf.h` — both enabled in host build and ESP-IDF), Go (lattice-protocol codegen), GoogleTest + sim harness (`tests/`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-16-multihop-routing-e2e-crypto-design.md` (Sections 1, 2, 6, 8; Phase 1 of Section 9).
- `PROTO_VERSION = 3`; flag day — no v2 compatibility (`Mesh.h:34`).
- Nonce (12B): `epoch_num(4, LE) || seq_num(2, LE) || origin_mac(6)`. Nonce reuse is fatal — the seq-wrap epoch bump (Task 7) is mandatory.
- AAD (24B): `proto_version(1) || message_type(1) || data_type(4, LE) || origin_mac(6) || target_mac(6) || epoch_num(4, LE) || seq_num(2, LE)`. Mutable-in-flight fields (`last_hop`, `hop_count`, `route_len`, `route_path`) are NEVER in the AAD.
- HKDF labels: `"lattice-e2e-up-v3"` (node→master) and `"lattice-e2e-down-v3"` (master→node). Uplink seals with `k_up` only in Phase 1.
- Sealed types: `MESH_TYPE_ADAPTER_DATA` and `MESH_TYPE_ROUTE_REPORT`, only when self-originated by a non-master node. Beacons, enrollment, JOIN_ACK, `SERIAL_CMD_BROADCAST`, and all downlink stay plaintext in Phase 1 (downlink seals in Phase 3).
- Decrypt/tag failure → quiet drop + `LOG_WARN` (finding-#9 pattern; never `err::fail` in a periodic path).
- New compile-time knob: `LATTICE_E2E_KEYCACHE_MAX` (default `MAX_PEERS`) in `main/project_config.h`.
- All firmware test hooks `#ifdef UNIT_TEST`-gated (PR #28 convention).
- Verification loop: `cmake --build tests/build --parallel` then `ctest --test-dir tests/build --output-on-failure`. Configure once with `cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release`. There is no device build in CI.
- lattice-protocol CI requires `go generate ./...` output committed (`git diff --exit-code c/`).

---

### Task 1: Protocol v3 wire format (lattice-protocol repo)

**Files:**
- Modify: `../lattice-protocol/message/message.go`
- Regenerate: `../lattice-protocol/c/mesh_message.h`, `../lattice-protocol/proto/mesh.proto` (via `go generate`)

**Interfaces:**
- Produces: packed `mesh_message` with new trailing fields `route_len(u8)`, `route_path[60]`, `auth_tag[16]`, `secondary_master_mac[6]`, `secondary_public_key[32]`; `sizeof(mesh_message) == 242`. All later tasks rely on `msg.auth_tag`.
- New fields append AFTER `enrollment_public_key` — the existing 127-byte prefix layout is unchanged.

- [ ] **Step 1: Create feature branch in ../lattice-protocol**

```bash
cd ../lattice-protocol && git checkout -b feat/proto-v3-e2e-aead
```

- [ ] **Step 2: Add v3 fields to the struct**

In `message/message.go`, replace the struct and constant with:

```go
// MeshMessage is the 242-byte packed wire-format frame for the Lattice mesh (protocol v3).
// Field order matches the packed C struct — do not reorder without updating the static_assert.
// v3 additions (route_len..secondary_public_key) append after enrollment_public_key so the
// 127-byte v2 prefix layout is unchanged.
type MeshMessage struct {
	ProtoVersion        uint8    `c:"uint8_t"     proto:"10,uint32"`
	MessageType         uint8    `c:"uint8_t"     proto:"1,uint32"`
	DataType            int32    `c:"int32_t"     proto:"2,sint32"`
	OriginMacAddress    [6]byte  `c:"uint8_t[6]"  proto:"3,bytes"`
	TargetMacAddress    [6]byte  `c:"uint8_t[6]"  proto:"4,bytes"`
	LastHopMacAddress   [6]byte  `c:"uint8_t[6]"  proto:"5,bytes"`
	Data                [64]byte `c:"uint8_t[64]" proto:"6,bytes,optional"`
	HopCount            uint8    `c:"uint8_t"     proto:"7,uint32"`
	EpochNum            uint32   `c:"uint32_t"    proto:"8,uint32"`
	SeqNum              uint16   `c:"uint16_t"    proto:"9,uint32"`
	EnrollmentPublicKey [32]byte `c:"uint8_t[32]" proto:"11,bytes,optional,public_key"`
	// v3: downlink source route (Phase 3) / uplink path accumulator. 60 = MAX_HOPS(10) × 6.
	RouteLen  uint8    `c:"uint8_t"     proto:"12,uint32,optional"`
	RoutePath [60]byte `c:"uint8_t[60]" proto:"13,bytes,optional"`
	// v3: ChaCha20-Poly1305 tag over data[64] (E2E AEAD).
	AuthTag [16]byte `c:"uint8_t[16]" proto:"14,bytes,optional"`
	// v3: dual-master provisioning in JOIN_ACK (Phase 4). Zero elsewhere.
	SecondaryMasterMac  [6]byte  `c:"uint8_t[6]"  proto:"15,bytes,optional"`
	SecondaryPublicKey  [32]byte `c:"uint8_t[32]" proto:"16,bytes,optional"`
}

// WireSize is the expected packed byte size — enforced by static_assert in the generated C header.
// 127 (v2 prefix) + 1 + 60 + 16 + 6 + 32 = 242. Must stay ≤ 250 (ESP-NOW frame limit).
const WireSize = 242
```

- [ ] **Step 3: Regenerate and test**

```bash
cd ../lattice-protocol && go generate ./... && go test ./... && git diff --stat
```

Expected: tests PASS; diff shows `message/message.go`, `c/mesh_message.h`, `proto/mesh.proto`. Verify the generated header ends with `static_assert(sizeof(mesh_message) == 242, ...)` and the new fields appear after `enrollment_public_key`. If `gen-headers` rejects a tag form, match the exact tag syntax used by existing optional fields (`proto:"11,bytes,optional,public_key"` is the reference).

- [ ] **Step 4: Commit and push**

```bash
cd ../lattice-protocol && git add -A && \
git commit -m "feat: protocol v3 — route path, AEAD auth tag, dual-master JOIN_ACK fields" && \
git push -u origin feat/proto-v3-e2e-aead
```

(Push is required so the lattice-nodes CI can fetch the submodule commit.)

---

### Task 2: Submodule bump + PROTO_VERSION 3

**Files:**
- Modify: `main/lib/lattice-protocol` (submodule pointer)
- Modify: `main/src/mesh/Mesh.h:34` (`PROTO_VERSION`)

**Interfaces:**
- Consumes: Task 1's commit in ../lattice-protocol.
- Produces: `mesh_message` with `auth_tag` visible to firmware; all nodes stamp/require `proto_version == 3`.

- [ ] **Step 1: Create feature branch in lattice-nodes, point submodule at Task 1's commit**

```bash
git checkout -b feat/phase1-proto-v3-e2e-aead
cd main/lib/lattice-protocol && git fetch && git checkout feat/proto-v3-e2e-aead && cd ../../..
```

- [ ] **Step 2: Bump PROTO_VERSION**

In `main/src/mesh/Mesh.h:34` change the value `2` to `3` (keep the constant name and type).

- [ ] **Step 3: Build and run full suite**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```

Expected: ALL PASS. Every sim node compiles against the new struct and speaks v3; nothing checks the old 127-byte size outside the regenerated header. If a harness file fails to compile on struct size, fix it here — it was relying on layout it shouldn't.

- [ ] **Step 4: Commit**

```bash
git add main/lib/lattice-protocol main/src/mesh/Mesh.h
git commit -m "feat: bump lattice-protocol to proto v3 wire format; PROTO_VERSION=3"
```

---

### Task 3: E2ECrypto — shared secret + HKDF key derivation

**Files:**
- Create: `main/src/mesh/E2ECrypto.h`
- Create: `tests/unit/test_e2e_crypto.cpp`
- Modify: `tests/CMakeLists.txt` (add unit target)

**Interfaces:**
- Consumes: `crypto::generateKeypair()` from `MeshCrypto.h` (test only).
- Produces:
  - `lattice::mesh::crypto::computeSharedSecret(const uint8_t ownPriv32[32], const uint8_t peerPub32[32], uint8_t secret32Out[32])` — X25519 ECDH, `err::fatal` on mbedtls failure (same pattern/digits as `derivePeerLMK`).
  - `lattice::mesh::crypto::deriveE2EKeys(const uint8_t ownPriv32[32], const uint8_t peerPub32[32], uint8_t kUp32Out[32], uint8_t kDown32Out[32])`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_e2e_crypto.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/MeshCrypto.h"
#include "src/mesh/E2ECrypto.h"

using namespace lattice::mesh::crypto;

TEST(E2ECrypto, SharedSecretIsSymmetric) {
  uint8_t privA[32], pubA[32], privB[32], pubB[32];
  generateKeypair(privA, pubA);
  generateKeypair(privB, pubB);
  uint8_t sAB[32], sBA[32];
  computeSharedSecret(privA, pubB, sAB);
  computeSharedSecret(privB, pubA, sBA);
  EXPECT_EQ(0, memcmp(sAB, sBA, 32));
  // Sanity: secret is not all-zero
  uint8_t zero[32] = {};
  EXPECT_NE(0, memcmp(sAB, zero, 32));
}

TEST(E2ECrypto, DerivedKeysAreSymmetricAndDirectionSplit) {
  uint8_t privA[32], pubA[32], privB[32], pubB[32];
  generateKeypair(privA, pubA);
  generateKeypair(privB, pubB);
  uint8_t upA[32], downA[32], upB[32], downB[32];
  deriveE2EKeys(privA, pubB, upA, downA);
  deriveE2EKeys(privB, pubA, upB, downB);
  EXPECT_EQ(0, memcmp(upA, upB, 32));     // both sides agree on k_up
  EXPECT_EQ(0, memcmp(downA, downB, 32)); // and on k_down
  EXPECT_NE(0, memcmp(upA, downA, 32));   // directions differ
}

TEST(E2ECrypto, DifferentPeersDifferentKeys) {
  uint8_t priv[32], pub[32], privB[32], pubB[32], privC[32], pubC[32];
  generateKeypair(priv, pub);
  generateKeypair(privB, pubB);
  generateKeypair(privC, pubC);
  uint8_t upB[32], downB[32], upC[32], downC[32];
  deriveE2EKeys(priv, pubB, upB, downB);
  deriveE2EKeys(priv, pubC, upC, downC);
  EXPECT_NE(0, memcmp(upB, upC, 32));
}
```

Add the target to `tests/CMakeLists.txt` next to the existing `add_unit_test` calls (copy their exact form; the macro at `tests/CMakeLists.txt:82-96` handles sources/links):

```cmake
add_unit_test(test_e2e_crypto unit/test_e2e_crypto.cpp)
```

(If the macro's real signature differs, mirror an existing call like `test_pir_adapter` exactly.)

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --target test_e2e_crypto --parallel
```

Expected: FAIL — `E2ECrypto.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `main/src/mesh/E2ECrypto.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"

namespace lattice {
namespace mesh {
namespace crypto {

// X25519 ECDH shared secret. Same mbedtls flow as derivePeerLMK (MeshCrypto.h),
// without the LMK KDF step. err::fatal digits 20-25 (LMK path uses 10-19).
inline void computeSharedSecret(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                                uint8_t* secret32Out) {
  mbedtls_ecdh_context ecdh;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecdh_init(&ecdh);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  const char* pers = "lattice_ecdh_e2e";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const uint8_t*>(pers), strlen(pers));
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 20,
                        "MESH: computeSharedSecret — ctr_drbg_seed failed");
  }
  ret = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_CURVE25519);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 21,
                        "MESH: computeSharedSecret — ecdh_setup failed");
  }
  ret = mbedtls_mpi_read_binary(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(d), ownPrivateKey32,
      32);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 22,
                        "MESH: computeSharedSecret — mpi_read_binary (private key) failed");
  }
  ret = mbedtls_mpi_read_binary(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(X),
      peerPublicKey32, 32);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 23,
                        "MESH: computeSharedSecret — mpi_read_binary (peer public key) failed");
  }
  ret = mbedtls_mpi_lset(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(Z),
      1);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 24,
                        "MESH: computeSharedSecret — mpi_lset (Qp.Z) failed");
  }
  size_t outLen = 0;
  ret = mbedtls_ecdh_calc_secret(&ecdh, &outLen, secret32Out, 32, mbedtls_ctr_drbg_random,
                                 &ctr_drbg);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 25,
                        "MESH: computeSharedSecret — ecdh_calc_secret failed");
  }
  mbedtls_ecdh_free(&ecdh);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
}

// Direction-split E2E keys (spec §2): HKDF-SHA256 over the ECDH secret.
// k_up seals node→master payloads, k_down master→node.
inline void deriveE2EKeys(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                          uint8_t* kUp32Out, uint8_t* kDown32Out) {
  uint8_t secret[32];
  computeSharedSecret(ownPrivateKey32, peerPublicKey32, secret);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  static const uint8_t upLabel[] = "lattice-e2e-up-v3";
  static const uint8_t downLabel[] = "lattice-e2e-down-v3";
  int ret = mbedtls_hkdf(md, nullptr, 0, secret, 32, upLabel, sizeof(upLabel) - 1, kUp32Out, 32);
  if (ret == 0) {
    ret = mbedtls_hkdf(md, nullptr, 0, secret, 32, downLabel, sizeof(downLabel) - 1, kDown32Out,
                       32);
  }
  memset(secret, 0, sizeof(secret));
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 26,
                        "MESH: deriveE2EKeys — hkdf failed");
  }
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build tests/build --target test_e2e_crypto --parallel && ctest --test-dir tests/build -R e2e_crypto --output-on-failure
```

Expected: 3 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add main/src/mesh/E2ECrypto.h tests/unit/test_e2e_crypto.cpp tests/CMakeLists.txt
git commit -m "feat: E2E key derivation — X25519 shared secret + HKDF-SHA256 direction-split keys"
```

---

### Task 4: E2ECrypto — seal/open payload (ChaCha20-Poly1305)

**Files:**
- Modify: `main/src/mesh/E2ECrypto.h`
- Modify: `tests/unit/test_e2e_crypto.cpp`

**Interfaces:**
- Produces:
  - `crypto::sealPayload(const uint8_t key32[32], mesh_message& msg)` — encrypts `msg.data` in place, writes `msg.auth_tag`. Returns `bool` (false on mbedtls error).
  - `crypto::openPayload(const uint8_t key32[32], mesh_message& msg)` — decrypts in place; returns `false` on tag mismatch (caller drops quietly).
  - Nonce/AAD layout per Global Constraints.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_e2e_crypto.cpp` (add `#include "mesh_message.h"` — the include path already resolves lattice-protocol headers for existing tests; mirror how `Mesh.cpp` includes it):

```cpp
static mesh_message makeMsg() {
  mesh_message m = {};
  m.proto_version = 3;
  m.message_type = 0; // MESH_TYPE_ADAPTER_DATA
  m.data_type = 1;
  const uint8_t origin[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t target[6] = {0x02, 0, 0, 0, 0, 0xAA};
  memcpy(m.origin_mac_address, origin, 6);
  memcpy(m.target_mac_address, target, 6);
  m.epoch_num = 7;
  m.seq_num = 42;
  for (int i = 0; i < 64; ++i) m.data[i] = static_cast<uint8_t>(i);
  return m;
}

TEST(E2EAead, SealOpenRoundtrip) {
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0xA0 + i);
  mesh_message m = makeMsg();
  mesh_message orig = m;
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  EXPECT_NE(0, memcmp(m.data, orig.data, 64)); // actually encrypted
  ASSERT_TRUE(lattice::mesh::crypto::openPayload(key, m));
  EXPECT_EQ(0, memcmp(m.data, orig.data, 64));
}

TEST(E2EAead, TamperedCiphertextFailsOpen) {
  uint8_t key[32] = {1};
  mesh_message m = makeMsg();
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  m.data[10] ^= 0x01;
  EXPECT_FALSE(lattice::mesh::crypto::openPayload(key, m));
}

TEST(E2EAead, TamperedAadFieldFailsOpen) {
  uint8_t key[32] = {2};
  mesh_message m = makeMsg();
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  m.data_type = 99; // AAD-bound field
  EXPECT_FALSE(lattice::mesh::crypto::openPayload(key, m));
}

TEST(E2EAead, MutableFieldsNotBound) {
  uint8_t key[32] = {3};
  mesh_message m = makeMsg();
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  m.hop_count = 5;                              // relays rewrite these
  memset(m.last_hop_mac_address, 0xBB, 6);
  m.route_len = 2;
  EXPECT_TRUE(lattice::mesh::crypto::openPayload(key, m));
}

// RFC 8439 §2.8.2 known-answer vector, exercised against the same mbedtls
// primitive sealPayload uses (spec §7: AEAD KAT). Guards against a broken or
// misconfigured chachapoly build on either host or target toolchains.
TEST(E2EAead, Rfc8439KnownAnswer) {
  const uint8_t key[32] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
                           0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
                           0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
  const uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
  const uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
  const char* plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you "
      "only one tip for the future, sunscreen would be it.";
  const uint8_t expectedTag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
                                   0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
  uint8_t ct[114], tag[16];
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  ASSERT_EQ(0, mbedtls_chachapoly_setkey(&ctx, key));
  ASSERT_EQ(0, mbedtls_chachapoly_encrypt_and_tag(
                   &ctx, 114, nonce, aad, sizeof(aad),
                   reinterpret_cast<const uint8_t*>(plaintext), ct, tag));
  mbedtls_chachapoly_free(&ctx);
  EXPECT_EQ(0, memcmp(tag, expectedTag, 16));
  EXPECT_EQ(0x64, ct[0]); // first ciphertext byte per RFC 8439 §2.8.2
}

TEST(E2EAead, WrongKeyFailsOpen) {
  uint8_t key[32] = {4}, wrong[32] = {5};
  mesh_message m = makeMsg();
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  EXPECT_FALSE(lattice::mesh::crypto::openPayload(wrong, m));
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build tests/build --target test_e2e_crypto --parallel
```

Expected: FAIL — `sealPayload` not declared.

- [ ] **Step 3: Write the implementation**

Append to `main/src/mesh/E2ECrypto.h` (inside the namespaces; add `#include <mbedtls/chachapoly.h>` and `#include "mesh_message.h"` at the top with the existing includes — use the same include form as `Mesh.h` uses for the protocol header):

```cpp
// AEAD framing (spec §1/§2).
// Nonce (12B): epoch(4 LE) || seq(2 LE) || origin_mac(6) — unique per key given the
// boot-epoch counter and the seq-wrap epoch bump.
// AAD (24B): version, type, data_type, origin, target, epoch, seq — immutable fields only.
constexpr size_t E2E_AAD_LEN = 24;
constexpr size_t E2E_NONCE_LEN = 12;

inline void buildNonce(const mesh_message& msg, uint8_t nonce[E2E_NONCE_LEN]) {
  nonce[0] = static_cast<uint8_t>(msg.epoch_num);
  nonce[1] = static_cast<uint8_t>(msg.epoch_num >> 8);
  nonce[2] = static_cast<uint8_t>(msg.epoch_num >> 16);
  nonce[3] = static_cast<uint8_t>(msg.epoch_num >> 24);
  nonce[4] = static_cast<uint8_t>(msg.seq_num);
  nonce[5] = static_cast<uint8_t>(msg.seq_num >> 8);
  memcpy(nonce + 6, msg.origin_mac_address, 6);
}

inline void buildAad(const mesh_message& msg, uint8_t aad[E2E_AAD_LEN]) {
  aad[0] = msg.proto_version;
  aad[1] = msg.message_type;
  aad[2] = static_cast<uint8_t>(msg.data_type);
  aad[3] = static_cast<uint8_t>(msg.data_type >> 8);
  aad[4] = static_cast<uint8_t>(msg.data_type >> 16);
  aad[5] = static_cast<uint8_t>(msg.data_type >> 24);
  memcpy(aad + 6, msg.origin_mac_address, 6);
  memcpy(aad + 12, msg.target_mac_address, 6);
  aad[18] = static_cast<uint8_t>(msg.epoch_num);
  aad[19] = static_cast<uint8_t>(msg.epoch_num >> 8);
  aad[20] = static_cast<uint8_t>(msg.epoch_num >> 16);
  aad[21] = static_cast<uint8_t>(msg.epoch_num >> 24);
  aad[22] = static_cast<uint8_t>(msg.seq_num);
  aad[23] = static_cast<uint8_t>(msg.seq_num >> 8);
}

// Encrypts msg.data in place and writes msg.auth_tag. Returns false on mbedtls error.
inline bool sealPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int ret = mbedtls_chachapoly_setkey(&ctx, key32);
  if (ret == 0) {
    ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, sizeof(msg.data), nonce, aad, E2E_AAD_LEN,
                                             msg.data, msg.data, msg.auth_tag);
  }
  mbedtls_chachapoly_free(&ctx);
  return ret == 0;
}

// Decrypts msg.data in place, verifying msg.auth_tag. Returns false on tag mismatch
// or mbedtls error — callers drop the frame quietly (finding-#9 pattern).
inline bool openPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int ret = mbedtls_chachapoly_setkey(&ctx, key32);
  if (ret == 0) {
    ret = mbedtls_chachapoly_auth_decrypt(&ctx, sizeof(msg.data), nonce, aad, E2E_AAD_LEN,
                                          msg.auth_tag, msg.data, msg.data);
  }
  mbedtls_chachapoly_free(&ctx);
  return ret == 0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_e2e_crypto --parallel && ctest --test-dir tests/build -R e2e_crypto --output-on-failure
```

Expected: 9 tests PASS (3 key-derivation + 6 AEAD).

- [ ] **Step 5: Commit**

```bash
git add main/src/mesh/E2ECrypto.h tests/unit/test_e2e_crypto.cpp
git commit -m "feat: ChaCha20-Poly1305 payload seal/open with header-bound AAD"
```

---

### Task 5: E2EKeyStore — per-peer derived-key cache

**Files:**
- Create: `main/src/mesh/E2EKeyStore.h`
- Create: `tests/unit/test_e2e_keystore.cpp`
- Modify: `main/project_config.h` (add knob), `tests/CMakeLists.txt` (add target)

**Interfaces:**
- Consumes: `crypto::deriveE2EKeys` (Task 3).
- Produces:
  - `class lattice::mesh::E2EKeyStore` with
    `bool getKeys(const uint8_t mac[6], const uint8_t ownPriv32[32], const uint8_t peerPub32[32], const uint8_t** kUpOut, const uint8_t** kDownOut)`
    — returns cached keys or derives+caches; returns `false` (and derives nothing) when `peerPub32` is null or all-zero. Round-robin overwrite when full.
  - `void clear()` — drops all cached keys (call on re-key).
  - `LATTICE_E2E_KEYCACHE_MAX` in `lattice::config` (default `= MAX_PEERS`).

- [ ] **Step 1: Add the config knob**

In `main/project_config.h`, next to the other mesh constants (near `MAX_HOPS` at line ~115):

```cpp
// E2E AEAD derived-key cache entries (spec §2). One entry per (peer, master) pair;
// masters need one per enrolled node, leaves need one per master. Default: MAX_PEERS.
inline constexpr size_t LATTICE_E2E_KEYCACHE_MAX = lattice::utils::EEPROM_SIZES::MAX_PEERS;
```

(If `MAX_PEERS` isn't visible in this header, use the literal `10` with a comment `// = MAX_PEERS`.)

- [ ] **Step 2: Write the failing test**

Create `tests/unit/test_e2e_keystore.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/MeshCrypto.h"
#include "src/mesh/E2EKeyStore.h"

using namespace lattice::mesh;

TEST(E2EKeyStore, DerivesAndCaches) {
  uint8_t priv[32], pub[32], peerPriv[32], peerPub[32];
  crypto::generateKeypair(priv, pub);
  crypto::generateKeypair(peerPriv, peerPub);
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
  E2EKeyStore store;
  const uint8_t *up1, *down1, *up2, *down2;
  ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up1, &down1));
  ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up2, &down2));
  EXPECT_EQ(up1, up2); // cache hit: same storage, no re-derivation
  EXPECT_EQ(0, memcmp(down1, down2, 32));
}

TEST(E2EKeyStore, RejectsZeroPubkey) {
  uint8_t priv[32], pub[32];
  crypto::generateKeypair(priv, pub);
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 2};
  uint8_t zeroPub[32] = {};
  E2EKeyStore store;
  const uint8_t *up, *down;
  EXPECT_FALSE(store.getKeys(mac, priv, zeroPub, &up, &down));
  EXPECT_FALSE(store.getKeys(mac, priv, nullptr, &up, &down));
}

TEST(E2EKeyStore, OverwritesWhenFullAndClearWorks) {
  uint8_t priv[32], pub[32], peerPriv[32], peerPub[32];
  crypto::generateKeypair(priv, pub);
  crypto::generateKeypair(peerPriv, peerPub);
  E2EKeyStore store;
  const uint8_t *up, *down;
  // Fill beyond capacity — must not crash, oldest entries overwritten
  for (int i = 0; i < static_cast<int>(lattice::config::LATTICE_E2E_KEYCACHE_MAX) + 3; ++i) {
    uint8_t mac[6] = {2, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up, &down));
  }
  store.clear();
  const uint8_t mac0[6] = {2, 0, 0, 0, 0, 0};
  ASSERT_TRUE(store.getKeys(mac0, priv, peerPub, &up, &down)); // re-derives after clear
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_unit_test(test_e2e_keystore unit/test_e2e_keystore.cpp)
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release && cmake --build tests/build --target test_e2e_keystore --parallel
```

Expected: FAIL — `E2EKeyStore.h: No such file or directory`.

- [ ] **Step 4: Write the implementation**

Create `main/src/mesh/E2EKeyStore.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include "E2ECrypto.h"
#include "../../project_config.h"

namespace lattice {
namespace mesh {

// RAM-only cache of derived E2E key pairs, one entry per peer MAC (spec §2).
// Derivation costs an X25519 exchange (~ms on ESP32) — cache so the periodic
// uplink path never re-derives. Round-robin overwrite when full; wrong evictions
// only cost a re-derivation.
class E2EKeyStore {
public:
  bool getKeys(const uint8_t mac[6], const uint8_t* ownPriv32, const uint8_t* peerPub32,
               const uint8_t** kUpOut, const uint8_t** kDownOut) {
    for (size_t i = 0; i < config::LATTICE_E2E_KEYCACHE_MAX; ++i) {
      if (entries[i].valid && memcmp(entries[i].mac, mac, 6) == 0) {
        *kUpOut = entries[i].kUp;
        *kDownOut = entries[i].kDown;
        return true;
      }
    }
    if (!peerPub32)
      return false;
    bool allZero = true;
    for (int i = 0; i < 32; ++i) {
      if (peerPub32[i] != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero)
      return false;
    Entry& e = entries[nextSlot];
    nextSlot = (nextSlot + 1) % config::LATTICE_E2E_KEYCACHE_MAX;
    crypto::deriveE2EKeys(ownPriv32, peerPub32, e.kUp, e.kDown);
    memcpy(e.mac, mac, 6);
    e.valid = true;
    *kUpOut = e.kUp;
    *kDownOut = e.kDown;
    return true;
  }

  void clear() {
    memset(entries, 0, sizeof(entries));
    nextSlot = 0;
  }

private:
  struct Entry {
    uint8_t mac[6];
    bool valid;
    uint8_t kUp[32];
    uint8_t kDown[32];
  };
  Entry entries[config::LATTICE_E2E_KEYCACHE_MAX]{};
  size_t nextSlot{0};
};

} // namespace mesh
} // namespace lattice
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build tests/build --target test_e2e_keystore --parallel && ctest --test-dir tests/build -R e2e_keystore --output-on-failure
```

Expected: 3 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add main/src/mesh/E2EKeyStore.h tests/unit/test_e2e_keystore.cpp tests/CMakeLists.txt main/project_config.h
git commit -m "feat: E2EKeyStore — cached per-peer HKDF key derivation"
```

---

### Task 6: Mesh integration — seal uplink, open at master

**Files:**
- Modify: `main/src/mesh/Mesh.h` (member + helper decls)
- Modify: `main/src/mesh/Mesh.cpp` (`transmitCore`, `processAdapterData`, `processRouteReport`)
- Create: `tests/e2e/scenarios/test_e2e_aead.cpp`
- Modify: `tests/CMakeLists.txt` (add scenario to `lattice_e2e` sources — mirror how existing scenarios are listed)

**Interfaces:**
- Consumes: `E2EKeyStore` (Task 5), `crypto::sealPayload`/`openPayload` (Task 4), `enrollment.getPrivateKey()` (`Enrollment.h:27`), `peers.find(mac)->publicKey` (`PeerRegistry.h:35`).
- Produces: sealed-on-air uplink. Rule (both directions of the same predicate):
  - **Seal** in `transmitCore` when: message is self-originated (`origin == deviceMacAddress`), node is not master, type is `MESH_TYPE_ADAPTER_DATA` or `MESH_TYPE_ROUTE_REPORT`, and master keys are available.
  - **Open** on local delivery when: node is master and frame is a self-targeted `ADAPTER_DATA`/`ROUTE_REPORT`. Failure → `LOG_WARN` + drop.

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e/scenarios/test_e2e_aead.cpp`. Follow the exact setup idiom of `tests/e2e/scenarios/test_pir_dataflow_e2e.cpp` (bus/node/hub construction, enrollment approval, cycle-stepping helpers) — copy its fixture and adapt. The scenario content:

```cpp
// Test 1: SealedUplinkDeliversPlaintextToHub
//  - master + leaf linked on VirtualBus; enroll leaf via FakeHub approval (existing idiom)
//  - leaf sends PIR adapter data (same trigger as test_pir_dataflow_e2e)
//  - capture the frame ON THE BUS: assert its data[] differs from the known
//    plaintext opcode layout (i.e. data[0] != OP_PIR_* expected byte) — proves sealed in transit
//  - run cycles; assert FakeHub received the decoded adapter data with the
//    ORIGINAL plaintext opcode — proves master opened before serial delivery

// Test 2: TamperedFrameIsDropped
//  - same setup; intercept the in-flight frame via the VirtualBus pending queue
//    (add a test-only mutator to VirtualBus if none exists: e.g.
//    `mesh_message* lastPending()` #ifdef-free — harness code, not firmware)
//  - flip one byte of msg.data, deliver
//  - assert FakeHub receives nothing and master logs/drops (no externalRecvCallback delivery)

// Test 3: ForgedFrameWithoutValidTagIsDropped
//  - construct a raw mesh_message impersonating the leaf (correct MACs/epoch/seq,
//    garbage auth_tag), inject via bus toward master
//  - assert FakeHub receives nothing
```

Write these as real GoogleTest cases against the harness API (read `tests/e2e/harness/VirtualBus.h`, `FakeHub.h`, `SimNode.h` first; reuse their existing accessors — only add a harness mutator if none exists).

- [ ] **Step 2: Run to verify the right failure**

```bash
cmake --build tests/build --target lattice_e2e --parallel && ctest --test-dir tests/build -R e2e_aead --output-on-failure
```

Expected: Test 1 FAILS at the "differs from plaintext" assertion (payload is still plaintext on the bus — feature not implemented). Tests 2–3 may incidentally pass or fail; only Test 1's failure mode matters here.

- [ ] **Step 3: Implement sealing in `transmitCore`**

In `main/src/mesh/Mesh.h`: add includes for `E2EKeyStore.h`; add private members and helpers:

```cpp
E2EKeyStore e2eKeys;
// Returns k_up/k_down for the current master (leaf side); false if not enrolled
// or master pubkey unknown.
bool masterE2EKeys(const uint8_t** kUp, const uint8_t** kDown);
// Returns keys for an enrolled origin peer (master side); false if unknown peer.
bool peerE2EKeys(const uint8_t* originMac, const uint8_t** kUp, const uint8_t** kDown);
static bool isSealedType(uint8_t messageType);
```

In `main/src/mesh/Mesh.cpp`:

```cpp
bool Mesh::isSealedType(uint8_t messageType) {
  return messageType == MESH_TYPE_ADAPTER_DATA || messageType == MESH_TYPE_ROUTE_REPORT;
}

bool Mesh::masterE2EKeys(const uint8_t** kUp, const uint8_t** kDown) {
  if (!enrollment.hasMasterMac)
    return false;
  PeerInfo* master = peers.find(currentMaster.mac);
  if (!master)
    return false;
  return e2eKeys.getKeys(master->mac, enrollment.getPrivateKey(), master->publicKey, kUp, kDown);
}

bool Mesh::peerE2EKeys(const uint8_t* originMac, const uint8_t** kUp, const uint8_t** kDown) {
  PeerInfo* peer = peers.find(originMac);
  if (!peer)
    return false;
  return e2eKeys.getKeys(peer->mac, enrollment.getPrivateKey(), peer->publicKey, kUp, kDown);
}
```

In `transmitCore` (`Mesh.cpp:315-346`), after the target override block (line 328) and BEFORE the routing lookup, insert:

```cpp
  // E2E seal (spec §1/§2): self-originated uplink payloads only. Relayed frames
  // (msgOverride with foreign origin) are already sealed — forward untouched.
  bool selfOriginated = (memcmp(msg.origin_mac_address, deviceMacAddress, 6) == 0);
  if (!isMaster && selfOriginated && isSealedType(msg.message_type)) {
    const uint8_t *kUp, *kDown;
    if (!masterE2EKeys(&kUp, &kDown) || !lattice::mesh::crypto::sealPayload(kUp, msg)) {
      Logger::logln("MESH", "E2E seal unavailable — uplink dropped", LogLevel::LOG_WARN);
      return;
    }
  }
```

**Sealing must happen after the final `target_mac_address` is set** — target is AAD-bound.

- [ ] **Step 4: Implement opening at the master**

In `processAdapterData` (`Mesh.cpp:592-638`): at the top of the local-delivery section (right after the relay branches return, before the `isConfigOpcode` check at line 619), insert:

```cpp
  // E2E open (spec §2): master unseals self-targeted uplink before local delivery.
  mesh_message opened = msg;
  bool needsOpen = isMaster && addressedToSelf && isSealedType(msg.message_type);
  if (needsOpen) {
    const uint8_t *kUp, *kDown;
    if (!peerE2EKeys(msg.origin_mac_address, &kUp, &kDown) ||
        !lattice::mesh::crypto::openPayload(kUp, opened)) {
      Logger::logln("MESH", "E2E open failed — frame dropped", LogLevel::LOG_WARN);
      return;
    }
  }
```

and switch the remainder of the local-delivery block (`isConfigOpcode` check, `externalRecvCallback` call) to use `opened` instead of `msg`. The broadcast-relay tail (`relayDownlink(msg)` at line 636) keeps using the original `msg`.

In `processRouteReport` (`Mesh.cpp:806-840`):
- Master branch: open the payload the same way (reuse the exact pattern above) before parsing `data[1]`/path bytes; drop on failure.
- Relay branch: **stop appending MACs to `msg.data`** — forward the frame unmodified except `hop_count`/`last_hop` (payload is sealed; path accumulation moves to the header `route_path` field in Phase 3 — spec §4). Leave a one-line comment citing the spec section.

- [ ] **Step 5: Run the new scenario + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```

Expected: ALL PASS, including all pre-existing e2e scenarios (enrollment, PIR dataflow, route report, replay, dual-master, hotswap, master-health, serial-robustness). The route-report e2e must still pass — direct-range reports carry an empty path (`data[1] = 0`), unaffected by the relay-append removal. If `test_route_report_e2e` asserts on relay path accumulation, update that assertion to expect an empty path and reference spec §4 (header accumulation lands in Phase 3).

- [ ] **Step 6: Commit**

```bash
git add main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/e2e/scenarios/test_e2e_aead.cpp tests/CMakeLists.txt
git commit -m "feat: seal uplink payloads E2E; master opens before delivery"
```

---

### Task 7: seq-wrap epoch bump

**Files:**
- Modify: `main/src/mesh/Mesh.cpp` (`buildMessage`, `Mesh.cpp:93-111`)
- Modify: `main/src/mesh/Mesh.h` (`#ifdef UNIT_TEST` hook)
- Create: `tests/e2e/scenarios/test_seq_wrap.cpp` (add to `lattice_e2e` sources)

**Interfaces:**
- Consumes: `replay.nextSeq()` (`ReplayCache.h:34`, wraps `0xFFFF → 0`), `EepromManager::{load,save}BootEpoch` (`EepromManager.h:144-145`).
- Produces: nonce uniqueness across seq wrap — `seq_num == 0` never goes on air; epoch increments and persists instead.

- [ ] **Step 1: Add the UNIT_TEST hook**

In `main/src/mesh/Mesh.h`, alongside the existing `#ifdef UNIT_TEST` hooks (grep `UNIT_TEST` in the header and match placement):

```cpp
#ifdef UNIT_TEST
  ReplayCache& testReplay() { return replay; }
#endif
```

- [ ] **Step 2: Write the failing test**

Create `tests/e2e/scenarios/test_seq_wrap.cpp` (same fixture idiom as Task 6's scenario):

```cpp
// SeqWrapBumpsEpochAndKeepsSealing
//  - enroll leaf with master (standard fixture)
//  - leaf.mesh().testReplay().txSeqNum = 0xFFFE;
//  - uint32_t epochBefore = leaf.mesh().testReplay().bootEpoch;
//  - trigger three uplink sends (PIR events), stepping cycles between each
//  - assert leaf.mesh().testReplay().bootEpoch == epochBefore + 1  (wrapped once)
//  - assert FakeHub received all three payloads decrypted (post-wrap frames still open —
//    proves master used the frame's epoch_num, and no frame went out with seq 0)
//  - assert the leaf's EEPROM mock persisted the bumped epoch
//    (leaf.ctx() exposes the EEPROM mock — read BOOT_EPOCH via EepromManager or ctx state)
```

Write as a real GoogleTest case. Run:

```bash
cmake --build tests/build --target lattice_e2e --parallel && ctest --test-dir tests/build -R seq_wrap --output-on-failure
```

Expected: FAIL — epoch unchanged (no wrap handling) and/or a frame with `seq_num == 0` reused a nonce.

- [ ] **Step 3: Implement the bump**

In `buildMessage` (`Mesh.cpp:93-111`), replace lines 108-109 with:

```cpp
  msg.seq_num = replay.nextSeq();
  if (msg.seq_num == 0) {
    // seq wrapped (spec §2): a reused (epoch, seq) pair would reuse an AEAD nonce.
    // Advance the persisted epoch and restart the sequence.
    uint32_t epoch = EepromManager::getInstance().loadBootEpoch() + 1;
    EepromManager::getInstance().saveBootEpoch(epoch);
    replay.bootEpoch = epoch;
    msg.seq_num = replay.nextSeq();
  }
  msg.epoch_num = replay.bootEpoch;
```

(Note: `epoch_num` assignment moves AFTER the wrap check so the frame carries the bumped epoch.)

- [ ] **Step 4: Run test + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```

Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
git add main/src/mesh/Mesh.h main/src/mesh/Mesh.cpp tests/e2e/scenarios/test_seq_wrap.cpp tests/CMakeLists.txt
git commit -m "fix: bump persisted epoch on seq wrap to prevent AEAD nonce reuse"
```

---

### Task 8: Remove per-peer LMK encryption

**Files:**
- Modify: `main/src/mesh/MeshCrypto.h` (delete `derivePeerLMK`; simplify `registerPeerWithEspNow`)
- Modify: callers if any pass key args that are now unused (grep `registerPeerWithEspNow` — `Mesh.cpp:718-760` area)

**Interfaces:**
- Consumes: nothing new.
- Produces: `crypto::registerPeerWithEspNow(const uint8_t mac[6])` — all ESP-NOW peers registered unencrypted (spec §2: E2E AEAD is the security boundary; unencrypted slots raise the peer cap from ~6 to 20). `generateKeypair` and the PMK (`esp_now_set_pmk`, `Mesh.cpp:182`) remain.

- [ ] **Step 1: Simplify registerPeerWithEspNow**

In `main/src/mesh/MeshCrypto.h`: delete `derivePeerLMK` entirely (lines 17-119) and replace `registerPeerWithEspNow` (lines 121-150) with:

```cpp
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
```

Remove the now-unused includes from `MeshCrypto.h` if nothing else in the header uses them (`generateKeypair` still needs ecp/entropy/ctr_drbg — keep those; `mbedtls/ecdh.h` and `mbedtls/sha256.h` likely become removable).

- [ ] **Step 2: Fix call sites**

```bash
grep -rn "registerPeerWithEspNow" main/ tests/
```

Update every call to the new single-arg signature (expected: `Mesh.cpp` in `registerPeerWithKey`, possibly harness/test code). The peer's public key stays in `PeerRegistry` (feeds E2E derivation) — only the ESP-NOW-layer usage is deleted.

- [ ] **Step 3: Build + full suite**

```bash
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```

Expected: ALL PASS.

- [ ] **Step 4: Commit**

```bash
git add main/src/mesh/MeshCrypto.h main/src/mesh/Mesh.cpp
git commit -m "refactor: drop per-peer LMK link encryption — E2E AEAD is the security boundary"
```

---

### Task 9: Finish — full verification + PRs

**Files:**
- Modify: `docs/design-gaps/multihop-data-uplink.md` (status note only)

- [ ] **Step 1: Full clean-build verification**

```bash
rm -rf tests/build && cmake -B tests/build tests/ -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --parallel && ctest --test-dir tests/build --output-on-failure
```

Expected: ALL PASS from scratch.

- [ ] **Step 2: Note Phase 1 in the gap doc**

In `docs/design-gaps/multihop-data-uplink.md`, under **Status**, append one line:

```markdown
**Update 2026-07-16:** Phase 1 (protocol v3 + E2E payload AEAD, spec
`docs/superpowers/specs/2026-07-16-multihop-routing-e2e-crypto-design.md`) landed —
keying groundwork done; the routing gap itself closes in Phase 2.
```

Commit:

```bash
git add docs/design-gaps/multihop-data-uplink.md
git commit -m "docs: note Phase 1 (proto v3 + E2E AEAD) in gap doc"
```

- [ ] **Step 3: Open PRs**

Use the superpowers:finishing-a-development-branch skill. Two PRs, lattice-protocol first:

1. `../lattice-protocol` `feat/proto-v3-e2e-aead` → main; after merge, tag per repo convention (v0.x.0 series).
2. `lattice-nodes` `feat/phase1-proto-v3-e2e-aead` → main; before opening, re-point the submodule at the MERGED lattice-protocol commit (`cd main/lib/lattice-protocol && git fetch && git checkout <merged-sha>`), re-run the suite, amend the bump commit (same pattern as PR #30).

---

## Deferred (explicitly NOT in Phase 1)

- nanopb regen (`main/src/mesh/serialization/mesh.pb.*`): new proto fields don't cross the serial link yet; nanopb skips unknown fields on decode. Regenerate in Phase 3 (route path to server) / Phase 4 (secondary keys from server) with nanopb-0.4.9.1.
- Downlink sealing (`SERIAL_CMD_BROADCAST`, `relayDownlink` paths) — Phase 3, needs source routing to make downlink unicast.
- Header `route_path` accumulation for route reports — Phase 3 (relay-side payload append removed in Task 6 with a spec-§4 comment).
- Multi-hop uplink (NeighborTable) — Phase 2. The two `DISABLED_` e2e tests stay disabled after Phase 1.
