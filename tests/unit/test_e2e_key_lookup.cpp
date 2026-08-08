#include <gtest/gtest.h>
#include "mesh/E2EKeyLookup.h"

using namespace lattice::mesh;

TEST(E2EKeyLookupTest, MasterE2EKeysFalseWhenNoMasterKnown) {
  MasterInfo currentMaster{};
  PeerRegistry peers;
  Enrollment enrollment;
  E2EKeyStore e2eKeys;
  const uint8_t *kUp, *kDown;
  EXPECT_FALSE(masterE2EKeys(currentMaster, peers, enrollment, e2eKeys, &kUp, &kDown));
}

TEST(E2EKeyLookupTest, PeerE2EKeysFalseWhenPeerUnknown) {
  PeerRegistry peers;
  Enrollment enrollment;
  E2EKeyStore e2eKeys;
  uint8_t unknownMac[6] = {9, 9, 9, 9, 9, 9};
  const uint8_t *kUp, *kDown;
  EXPECT_FALSE(peerE2EKeys(unknownMac, peers, enrollment, e2eKeys, &kUp, &kDown));
}
