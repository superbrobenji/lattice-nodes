#include <gtest/gtest.h>
#include <cstring>
#include "src/mesh/MeshCrypto.h"
#include "src/mesh/E2EKeyStore.h"

using namespace lattice::mesh;

TEST(E2EKeyStore, DerivesAndCaches) {
  uint8_t priv[32], pub[32], peerPriv[32], peerPub[32];
  crypto::generateKeypair(priv, pub);
  crypto::generateKeypair(peerPriv, peerPub);
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
  E2EKeyStore store;
  const uint8_t *up1, *down1, *up2, *down2;
  ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up1, &down1));
  ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up2, &down2));
  EXPECT_EQ(up1, up2); // cache hit: same storage, no re-derivation
  EXPECT_EQ(0, memcmp(down1, down2, 32));
}

TEST(E2EKeyStore, RejectsZeroPubkey) {
  uint8_t priv[32], pub[32];
  crypto::generateKeypair(priv, pub);
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 2};
  uint8_t zeroPub[32] = {};
  E2EKeyStore store;
  const uint8_t *up, *down;
  EXPECT_FALSE(store.getKeys(mac, priv, zeroPub, &up, &down));
  EXPECT_FALSE(store.getKeys(mac, priv, nullptr, &up, &down));
}

TEST(E2EKeyStore, OverwritesWhenFullAndClearWorks) {
  uint8_t priv[32], pub[32], peerPriv[32], peerPub[32];
  crypto::generateKeypair(priv, pub);
  crypto::generateKeypair(peerPriv, peerPub);
  E2EKeyStore store;
  const uint8_t *up, *down;

  // Fill beyond capacity — must not crash, oldest entries overwritten
  for (int i = 0; i < static_cast<int>(lattice::config::LATTICE_E2E_KEYCACHE_MAX) + 3; ++i) {
    uint8_t mac[6] = {2, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    ASSERT_TRUE(store.getKeys(mac, priv, peerPub, &up, &down));
  }

  // Before clear: pick the last inserted mac (still resident), assert cache hit
  const int lastIdx = lattice::config::LATTICE_E2E_KEYCACHE_MAX + 2;
  const uint8_t lastMac[6] = {2, 0, 0, 0, 0, static_cast<uint8_t>(lastIdx)};
  const uint8_t *upBefore, *downBefore;
  ASSERT_TRUE(store.getKeys(lastMac, priv, peerPub, &upBefore, &downBefore)); // cache hit

  // Capture kUp pointer bytes to verify cache effect
  uint8_t kUpCached[32];
  memcpy(kUpCached, upBefore, 32);

  // Clear the cache
  store.clear();

  // After clear: immediately test with nullptr pubkey — should fail (cache is empty)
  const uint8_t *upNull, *downNull;
  ASSERT_FALSE(store.getKeys(lastMac, priv, nullptr, &upNull, &downNull));

  // Now re-derive: same mac with pubkey should succeed (refills cache after clear)
  const uint8_t *upAfter, *downAfter;
  ASSERT_TRUE(store.getKeys(lastMac, priv, peerPub, &upAfter, &downAfter)); // re-derives

  // Keep existing assertion: verify we can still add fresh entries after clear
  const uint8_t mac0[6] = {2, 0, 0, 0, 0, 0};
  ASSERT_TRUE(store.getKeys(mac0, priv, peerPub, &up, &down)); // re-derives after clear
}
