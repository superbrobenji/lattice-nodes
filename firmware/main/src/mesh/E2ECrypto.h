#pragma once
#include <cstdint>
#include <cstring>
#include <sodium.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {
namespace crypto {

// Byte-order shim between this codebase's established Curve25519 key
// storage/wire convention and libsodium's native one.
//
// Pre-Task-2, generateKeypair() (MeshCrypto.h) and computeSharedSecret()
// (below) moved scalars and point coordinates in and out of mbedtls's
// generic bignum type via mbedtls_mpi_write_binary()/mbedtls_mpi_read_binary()
// directly (bypassing mbedtls's curve-aware helpers) — both serialize as
// BIG-ENDIAN. libsodium's crypto_scalarmult_curve25519{,_base}() take/return
// the RFC 7748-native LITTLE-ENDIAN 32-byte encoding. Both compute the same
// curve, but every already-generated key — device keypairs already persisted
// in EEPROM/NVS on shipped nodes, the e2e master pin fixture
// (MasterKeypairFixture.h), any operator-pinned master_pubkey_pin.h — is
// stored, AND transmitted on the wire in mesh_message.enrollment_public_key /
// secondary_public_key, in that big-endian order. Reversing at the libsodium
// call boundary keeps that wire/storage convention byte-for-byte unchanged.
//
// Verified against a real mbedtls 3.6 build (see task report): for a real,
// previously mbedtls-generated keypair, reverse(mbedtls_priv) fed to
// crypto_scalarmult_curve25519_base reproduces exactly reverse(mbedtls_pub);
// and reverse(mbedtls_priv) + reverse(peer_mbedtls_pub) fed to
// crypto_scalarmult_curve25519 reproduces mbedtls_ecdh_calc_secret's shared
// secret bytes directly, with NO reversal needed on that output — mbedtls's
// Montgomery-curve-aware calc_secret already emits the RFC 7748-native
// little-endian form, unlike the generic-MPI-based key read/write it doesn't
// go through.
inline void reverse32(const uint8_t* in, uint8_t* out) {
  for (int i = 0; i < 32; ++i) {
    out[i] = in[31 - i];
  }
}

// X25519 ECDH shared secret (Phase I Task 2: libsodium — was mbedtls ECDH,
// see MeshCrypto.h). Same raw-Curve25519 flow as derivePeerLMK used to be,
// without the LMK KDF step. err::fatal digits 20-25 (LMK path uses 10-19).
//
// ownPrivateKey32/peerPublicKey32 arrive in this codebase's big-endian
// storage/wire convention (see reverse32() above) — reversed into libsodium's
// native little-endian form before the scalarmult call. The output secret is
// used verbatim (no reversal) — see reverse32()'s comment for why.
//
// crypto_scalarmult_curve25519() internally applies the standard X25519
// clamp before the ladder — idempotent whether the (now-reversed) key came
// from a freshly generated keypair (Task 2) or a reversed already-clamped
// mbedtls-era key (see MeshCrypto.h::generateKeypair). It also rejects
// (returns non-zero, all-zero output) small-order/degenerate peer public
// keys — a safety property mbedtls's raw ECDH path did not enforce, but one
// that never triggers for legitimate peer keys, so it does not change
// on-wire behavior.
inline void computeSharedSecret(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                                uint8_t* secret32Out) {
  uint8_t privLE[32], peerPubLE[32];
  reverse32(ownPrivateKey32, privLE);
  reverse32(peerPublicKey32, peerPubLE);
  int ret = crypto_scalarmult_curve25519(secret32Out, privLE, peerPubLE);
  sodium_memzero(privLE, sizeof(privLE));
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 25,
                        "MESH: computeSharedSecret — scalarmult failed");
  }
}

// Direction-split E2E keys (spec §2): HKDF-SHA256 over the ECDH secret.
// k_up seals node→master payloads, k_down master→node.
inline void deriveE2EKeys(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                          uint8_t* kUp32Out, uint8_t* kDown32Out) {
  uint8_t secret[32];
  computeSharedSecret(ownPrivateKey32, peerPublicKey32, secret);

  static const char upLabel[] = "lattice-e2e-up-v3";
  static const char downLabel[] = "lattice-e2e-down-v3";

  // HKDF-Extract-then-Expand (RFC 5869), same as the mbedtls_hkdf() call this
  // replaces: salt = NULL/0 both times, so the PRK is identical whether
  // extracted once (here) or twice (old code called mbedtls_hkdf() — which
  // extracts internally — once per label). Expanding the same PRK against
  // each label byte-for-byte reproduces the old per-label output.
  uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  int ret = crypto_kdf_hkdf_sha256_extract(prk, nullptr, 0, secret, sizeof(secret));
  if (ret == 0) {
    ret = crypto_kdf_hkdf_sha256_expand(kUp32Out, 32, upLabel, sizeof(upLabel) - 1, prk);
  }
  if (ret == 0) {
    ret = crypto_kdf_hkdf_sha256_expand(kDown32Out, 32, downLabel, sizeof(downLabel) - 1, prk);
  }
  sodium_memzero(secret, sizeof(secret));
  sodium_memzero(prk, sizeof(prk));
  if (ret != 0) {
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

// Encrypts msg.data in place and writes msg.auth_tag. Returns false on libsodium error.
// msg.data and msg.auth_tag are not adjacent in mesh_message (route_len/route_path sit
// between them), so the _detached variant is used — it writes ciphertext and tag to
// independently-addressed buffers, matching mbedtls_chachapoly_encrypt_and_tag's shape.
inline bool sealPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  unsigned long long tagLen = 0;
  int ret = crypto_aead_chacha20poly1305_ietf_encrypt_detached(msg.data, msg.auth_tag, &tagLen,
                                                               msg.data, sizeof(msg.data), aad,
                                                               E2E_AAD_LEN, nullptr, nonce, key32);
  return ret == 0 && tagLen == sizeof(msg.auth_tag);
}

// Decrypts msg.data in place, verifying msg.auth_tag. Returns false on tag mismatch
// or libsodium error — callers drop the frame quietly (finding-#9 pattern).
inline bool openPayload(const uint8_t* key32, mesh_message& msg) {
  uint8_t nonce[E2E_NONCE_LEN], aad[E2E_AAD_LEN];
  buildNonce(msg, nonce);
  buildAad(msg, aad);
  int ret = crypto_aead_chacha20poly1305_ietf_decrypt_detached(
      msg.data, nullptr, msg.data, sizeof(msg.data), msg.auth_tag, aad, E2E_AAD_LEN, nonce, key32);
  return ret == 0;
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
