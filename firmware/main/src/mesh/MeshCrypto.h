#pragma once
#include <cstdint>
#include <cstring>
#include <sodium.h>
#include <esp_now.h>
#include "src/error/Error.h"
#include "src/error/ErrorCore.h"
#include "E2ECrypto.h" // for crypto::reverse32() — see its definition for why

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
// Phase I Task 2: libsodium — was mbedtls low-level ECP keygen. randombytes_buf()
// draws the raw 32-byte scalar (seeded via sodium_init(), see main.cpp) in
// libsodium's native little-endian convention; the X25519 clamp mbedtls used to
// bake into the stored private key is instead applied internally by
// crypto_scalarmult_curve25519{,_base}() on every use (E2ECrypto.h). Both priv
// and pub are then byte-reversed into the legacy big-endian storage/wire
// convention (see E2ECrypto.h::reverse32()'s comment) before being written
// out, so on-device keys generated before this swap — and this device's own
// future reloads of what it just generated — keep interpreting the same
// bytes the same way.
inline void generateKeypair(uint8_t* priv32Out, uint8_t* pub32Out) {
  uint8_t privLE[32], pubLE[32];
  randombytes_buf(privLE, 32);
  int ret = crypto_scalarmult_curve25519_base(pubLE, privLE);
  if (ret != 0) {
    lattice::err::fatal(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::MESH, 3,
                        "MESH: keypair gen — scalarmult_base failed");
  }
  reverse32(privLE, priv32Out);
  reverse32(pubLE, pub32Out);
  sodium_memzero(privLE, sizeof(privLE));
}

} // namespace crypto
} // namespace mesh
} // namespace lattice
