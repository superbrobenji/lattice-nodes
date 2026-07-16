#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/chachapoly.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "../../lib/lattice-protocol/c/mesh_message.h"

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

} // namespace crypto
} // namespace mesh
} // namespace lattice
