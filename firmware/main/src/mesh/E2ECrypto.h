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
