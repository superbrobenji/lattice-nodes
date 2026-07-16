#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/MeshCrypto.h"
#include "src/mesh/E2ECrypto.h"

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
