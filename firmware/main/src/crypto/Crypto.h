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
