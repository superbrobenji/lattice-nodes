#include <cstring>
#include <gtest/gtest.h>
#include "mesh/PeerEnrollment.h"

using namespace lattice::mesh;

TEST(PeerEnrollmentTest, AddPeerAppendsToRegistry) {
  PeerRegistry peers;
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  addPeer(mac, peers);
  EXPECT_EQ(peers.count(), 1u);
  EXPECT_NE(peers.find(mac), nullptr);
}

TEST(PeerEnrollmentTest, RegisterPeerWithKeyRejectsRekeyOfEstablishedKey) {
  PeerRegistry peers;
  Enrollment enrollment;
  uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  uint8_t key1[32];
  memset(key1, 0xAA, 32);
  uint8_t key2[32];
  memset(key2, 0xBB, 32);
  ASSERT_TRUE(registerPeerWithKey(mac, key1, /*allowRekey=*/false, peers, enrollment, false));
  ASSERT_TRUE(registerPeerWithKey(mac, key2, /*allowRekey=*/false, peers, enrollment, false));
  // Established (non-zero) key must not be replaced when allowRekey is false.
  EXPECT_EQ(memcmp(peers.find(mac)->publicKey, key1, 32), 0);
}
