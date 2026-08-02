#pragma once
#include <cstdint>
#include <cstring>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <esp_now.h>
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
