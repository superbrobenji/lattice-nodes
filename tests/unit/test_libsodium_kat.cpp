#include <gtest/gtest.h>
#include <cstring>
#include <sodium.h>

// Phase I Task 2 (item GG, full mbedtls drop): known-answer tests for the
// three libsodium primitives E2ECrypto.h / MeshCrypto.h / RouteMac.h now
// call directly (crypto_aead_chacha20poly1305_ietf_*, crypto_scalarmult_
// curve25519, crypto_kdf_hkdf_sha256_*). These pin the exact byte outputs
// published in the governing RFCs so a broken/misconfigured libsodium build
// on either host or ESP-IDF target toolchains fails loudly here rather than
// as a silent wire-format break downstream. Every expected byte string below
// was cross-checked against a standalone program linked against the same
// host libsodium install these tests link against (see task report) — not
// transcribed by hand from the RFC text.

class LibsodiumKat : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

// RFC 8439 §2.8.2 — ChaCha20-Poly1305 (IETF) AEAD known-answer test. Same
// key/nonce/aad/plaintext as E2EAead.Rfc8439KnownAnswer in test_e2e_crypto.cpp
// (which exercises sealPayload's detached-encrypt call); this test instead
// calls the libsodium primitive directly and checks the FULL 114-byte
// ciphertext (not just tag + first byte), plus a decrypt round trip.
TEST_F(LibsodiumKat, ChaCha20Poly1305Rfc8439) {
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

  uint8_t ct[114], tag[16];
  unsigned long long tagLen = 0;
  ASSERT_EQ(0, crypto_aead_chacha20poly1305_ietf_encrypt_detached(
                   ct, tag, &tagLen, reinterpret_cast<const uint8_t*>(plaintext), 114, aad,
                   sizeof(aad), nullptr, nonce, key));
  EXPECT_EQ(16u, tagLen);
  EXPECT_EQ(0, memcmp(ct, expectedCt, sizeof(ct)));
  EXPECT_EQ(0, memcmp(tag, expectedTag, sizeof(tag)));

  uint8_t pt[114];
  ASSERT_EQ(0, crypto_aead_chacha20poly1305_ietf_decrypt_detached(pt, nullptr, ct, 114, tag, aad,
                                                                  sizeof(aad), nonce, key));
  EXPECT_EQ(0, memcmp(pt, plaintext, 114));
}

// RFC 7748 §5.2, X25519 Test 1 — raw Curve25519 scalar multiplication, the
// primitive computeSharedSecret() (E2ECrypto.h) and generateKeypair()
// (MeshCrypto.h) both build on via crypto_scalarmult_curve25519{,_base}().
TEST_F(LibsodiumKat, X25519Rfc7748Test1) {
  uint8_t scalar[32], u[32], out[32];
  ASSERT_EQ(0, sodium_hex2bin(scalar, sizeof(scalar),
                              "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                              64, nullptr, nullptr, nullptr));
  ASSERT_EQ(0, sodium_hex2bin(u, sizeof(u),
                              "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                              64, nullptr, nullptr, nullptr));

  const uint8_t expected[32] = {0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea,
                                0x4d, 0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c,
                                0x71, 0xf7, 0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};

  ASSERT_EQ(0, crypto_scalarmult_curve25519(out, scalar, u));
  EXPECT_EQ(0, memcmp(out, expected, sizeof(expected)));
}

// RFC 5869 Appendix A.1, Test Case 1 — HKDF-SHA256 (Extract-then-Expand),
// the exact two-step call deriveE2EKeys() (E2ECrypto.h) makes via
// crypto_kdf_hkdf_sha256_extract/_expand. Uses the RFC's own IKM/salt/info,
// not the mesh-specific "lattice-e2e-*" labels (those aren't independently
// published anywhere to check against; direction-split coverage instead
// lives in E2ECrypto.DerivedKeysAreSymmetricAndDirectionSplit).
TEST_F(LibsodiumKat, HkdfSha256Rfc5869TestCase1) {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  const uint8_t salt[13] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                            0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
  const char info[10] = {'\xf0', '\xf1', '\xf2', '\xf3', '\xf4',
                         '\xf5', '\xf6', '\xf7', '\xf8', '\xf9'};

  const uint8_t expectedPrk[32] = {0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf, 0x0d, 0xdc, 0x3f,
                                   0x0d, 0xc4, 0x7b, 0xba, 0x63, 0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f,
                                   0x9c, 0x31, 0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5};
  const uint8_t expectedOkm[42] = {0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f,
                                   0x64, 0xd0, 0x36, 0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a,
                                   0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf, 0x34,
                                   0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65};

  uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  ASSERT_EQ(0, crypto_kdf_hkdf_sha256_extract(prk, salt, sizeof(salt), ikm, sizeof(ikm)));
  EXPECT_EQ(0, memcmp(prk, expectedPrk, sizeof(expectedPrk)));

  uint8_t okm[42];
  ASSERT_EQ(0, crypto_kdf_hkdf_sha256_expand(okm, sizeof(okm), info, sizeof(info), prk));
  EXPECT_EQ(0, memcmp(okm, expectedOkm, sizeof(expectedOkm)));
}
