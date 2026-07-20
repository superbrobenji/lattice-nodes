#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/MeshCrypto.h"
#include "src/mesh/E2ECrypto.h"
#include "lattice-protocol/c/mesh_message.h"
#include <mbedtls/chachapoly.h>

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

// AEAD tests (6 new tests for ChaCha20-Poly1305 seal/open)

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
  EXPECT_EQ(0xd3, ct[0]); // first ciphertext byte per RFC 8439 §2.8.2
}

TEST(E2EAead, WrongKeyFailsOpen) {
  uint8_t key[32] = {4}, wrong[32] = {5};
  mesh_message m = makeMsg();
  ASSERT_TRUE(lattice::mesh::crypto::sealPayload(key, m));
  EXPECT_FALSE(lattice::mesh::crypto::openPayload(wrong, m));
}
