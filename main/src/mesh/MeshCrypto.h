#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <esp_now.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"

namespace lattice {
namespace mesh {
namespace crypto {

// Derive a 16-byte LMK for a peer using ECDH + SHA256.
// LMK = SHA256(ECDH_shared_secret || "lattice-lmk")[0:16]
inline void derivePeerLMK(const uint8_t* ownPrivateKey32, const uint8_t* peerPublicKey32,
                          uint8_t* lmk16Out) {
  mbedtls_ecdh_context ecdh;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecdh_init(&ecdh);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  int ret = 0;

  const char* pers = "lattice_ecdh";
  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              reinterpret_cast<const uint8_t*>(pers), strlen(pers));
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 10,
                        "MESH: derivePeerLMK — ctr_drbg_seed failed");
  }

  ret = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_CURVE25519);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 11,
                        "MESH: derivePeerLMK — ecdh_setup failed");
  }

  // Load own private key and peer public key (X coordinate only for Curve25519)
  ret = mbedtls_mpi_read_binary(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(d), ownPrivateKey32,
      32);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 12,
                        "MESH: derivePeerLMK — mpi_read_binary (private key) failed");
  }

  ret = mbedtls_mpi_read_binary(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(X),
      peerPublicKey32, 32);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 13,
                        "MESH: derivePeerLMK — mpi_read_binary (peer public key) failed");
  }

  ret = mbedtls_mpi_lset(
      &ecdh.MBEDTLS_PRIVATE(ctx).MBEDTLS_PRIVATE(mbed_ecdh).MBEDTLS_PRIVATE(Qp).MBEDTLS_PRIVATE(Z),
      1);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 14,
                        "MESH: derivePeerLMK — mpi_lset (Qp.Z) failed");
  }

  uint8_t sharedSecret[32] = {};
  size_t outLen = 0;
  ret = mbedtls_ecdh_calc_secret(&ecdh, &outLen, sharedSecret, sizeof(sharedSecret),
                                 mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 15,
                        "MESH: derivePeerLMK — ecdh_calc_secret failed");
  }

  mbedtls_ecdh_free(&ecdh);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);

  // KDF: SHA256(sharedSecret || "lattice-lmk"), take first 16 bytes
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);

  ret = mbedtls_sha256_starts(&sha, 0); // 0 = SHA-256, not SHA-224
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 16,
                        "MESH: derivePeerLMK — sha256_starts failed");
  }

  ret = mbedtls_sha256_update(&sha, sharedSecret, 32);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 17,
                        "MESH: derivePeerLMK — sha256_update (secret) failed");
  }

  const uint8_t label[] = "lattice-lmk";
  ret = mbedtls_sha256_update(&sha, label, sizeof(label) - 1);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 18,
                        "MESH: derivePeerLMK — sha256_update (label) failed");
  }

  uint8_t digest[32];
  ret = mbedtls_sha256_finish(&sha, digest);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 19,
                        "MESH: derivePeerLMK — sha256_finish failed");
  }

  mbedtls_sha256_free(&sha);

  memcpy(lmk16Out, digest, 16);

  // Zero sensitive buffers after use (Fix 2)
  memset(sharedSecret, 0, sizeof(sharedSecret));
  memset(digest, 0, sizeof(digest));
}

inline void registerPeerWithEspNow(const uint8_t mac[6], const uint8_t* ownPrivateKey32,
                                   const uint8_t* peerPublicKey32) {
  if (esp_now_is_peer_exist(mac))
    return;
  uint8_t lmk[16];
  bool hasPublicKey = false;
  if (peerPublicKey32) {
    // Check that public key is not all-zero (unset)
    for (int i = 0; i < 32; ++i) {
      if (peerPublicKey32[i] != 0x00) {
        hasPublicKey = true;
        break;
      }
    }
  }
  if (hasPublicKey && ownPrivateKey32) {
    derivePeerLMK(ownPrivateKey32, peerPublicKey32, lmk);
  } else {
    // Peer public key not yet known (pre-enrollment) — no encryption
    memset(lmk, 0, 16);
  }
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;
  info.encrypt = hasPublicKey;
  if (hasPublicKey)
    memcpy(info.lmk, lmk, 16);
  lattice::err::checkEsp(esp_now_add_peer(&info), lattice::utils::ErrorType::COMMUNICATION_FAIL,
                         "registerPeerWithEspNow: add_peer failed");
}

// Extract ONLY the key generation branch from Mesh::loadOrGenerateKeypair().
// The load-from-EEPROM branch and EEPROM save remain in loadOrGenerateKeypair().
inline void generateKeypair(uint8_t* priv32Out, uint8_t* pub32Out) {
  // Use the low-level ECP API directly to avoid the opaque ecdh context internals
  // that are private in the mbedTLS 3.x non-legacy context.
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point Q;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;

  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&Q);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  const char* pers = "lattice_keygen";
  int ret;
  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              reinterpret_cast<const uint8_t*>(pers), strlen(pers));
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 1,
                        "MESH: keypair gen — entropy seed failed");
  }

  ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 2,
                        "MESH: keypair gen — ecp_group_load failed");
  }

  ret = mbedtls_ecdh_gen_public(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 3,
                        "MESH: keypair gen — ecdh_gen_public failed");
  }

  // Export private scalar (d) — 32 bytes big-endian (NEVER printed to serial)
  mbedtls_mpi_write_binary(&d, priv32Out, 32);
  // Export public key X coordinate — 32 bytes (Curve25519 public key is X only)
  mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), pub32Out, 32);

  mbedtls_ecp_group_free(&grp);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
