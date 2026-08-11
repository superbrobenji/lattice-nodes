#include <gtest/gtest.h>
#include "mesh/PeerRegistry.h"

using namespace lattice::mesh;

static PeerInfo makePeer(uint8_t lastByte) {
  PeerInfo p{};
  memset(p.mac, 0, 6);
  p.mac[5] = lastByte;
  memset(p.publicKey, 0, 32);
  p.lastSeenMs = 0;
  return p;
}

TEST(PeerRegistryTest, CountReflectsAppends) {
  PeerRegistry reg;
  EXPECT_EQ(reg.count(), 0u);
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  EXPECT_EQ(reg.count(), 2u);
}

TEST(PeerRegistryTest, AtReturnsAppendedPeersInOrder) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  EXPECT_EQ(reg.at(0).mac[5], 1);
  EXPECT_EQ(reg.at(1).mac[5], 2);
}

TEST(PeerRegistryTest, IterationVisitsExactlyLiveEntries) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  reg.append(makePeer(2));
  reg.append(makePeer(3));
  std::vector<uint8_t> seen;
  for (const auto& p : reg) seen.push_back(p.mac[5]);
  EXPECT_EQ(seen, (std::vector<uint8_t>{1, 2, 3}));
}

TEST(PeerRegistryTest, IterationStopsAtCountNotCapacity) {
  PeerRegistry reg;
  reg.append(makePeer(1));
  size_t visited = 0;
  for (const auto& p : reg) { (void)p; ++visited; }
  EXPECT_EQ(visited, 1u);
}

TEST(PeerRegistryTest, ConstIterationWorks) {
  PeerRegistry reg;
  reg.append(makePeer(9));
  const PeerRegistry& constReg = reg;
  size_t visited = 0;
  for (const auto& p : constReg) { (void)p; ++visited; }
  EXPECT_EQ(visited, 1u);
}
