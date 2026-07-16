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
