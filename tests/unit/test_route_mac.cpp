#include <gtest/gtest.h>
#include <cstring>
#include "mesh/RouteMac.h"
#include "lib/lattice-protocol/c/mesh_message.h"

using namespace lattice::mesh::routemac;

static mesh_message makeMsg() {
  mesh_message m{};
  uint8_t origin[6]  = {0xAA,0,0,0,0,1};
  uint8_t target[6]  = {0xBB,0,0,0,0,2};
  memcpy(m.origin_mac_address, origin, 6);
  memcpy(m.target_mac_address, target, 6);
  m.epoch_num = 0x11223344;
  m.seq_num   = 0x5566;
  return m;
}

TEST(RouteMac, BuildHopContext_ByteExact) {
  mesh_message m = makeMsg();
  uint8_t prev[6] = {0x01,0x02,0x03,0x04,0x05,0x06};
  uint8_t self[6] = {0x11,0x22,0x33,0x44,0x55,0x66};
  uint8_t ctx[HOP_CTX_LEN];
  buildHopContext(m, prev, self, ctx);
  // origin(6) || dest(6) || epoch LE(4) || seq LE(2) || prev(6) || self(6)
  const uint8_t want[HOP_CTX_LEN] = {
    0xAA,0,0,0,0,1,            // origin
    0xBB,0,0,0,0,2,            // dest
    0x44,0x33,0x22,0x11,       // epoch LE
    0x66,0x55,                 // seq LE
    0x01,0x02,0x03,0x04,0x05,0x06,  // prev
    0x11,0x22,0x33,0x44,0x55,0x66   // self
  };
  ASSERT_EQ(0, memcmp(ctx, want, HOP_CTX_LEN));
}

TEST(RouteMac, ChainStep_StableForSameInput) {
  uint8_t secret[32]; for (int i=0; i<32; ++i) secret[i] = i;
  uint8_t ctx[HOP_CTX_LEN]; for (size_t i=0; i<HOP_CTX_LEN; ++i) ctx[i] = i;
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx, prev, out1);
  chainStep(secret, ctx, prev, out2);
  ASSERT_EQ(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenSecretChanges) {
  uint8_t s1[32] = {0}; s1[0] = 1;
  uint8_t s2[32] = {0}; s2[0] = 2;
  uint8_t ctx[HOP_CTX_LEN] = {0};
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(s1, ctx, prev, out1);
  chainStep(s2, ctx, prev, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenContextChanges) {
  uint8_t secret[32] = {0};
  uint8_t ctx1[HOP_CTX_LEN] = {0}; ctx1[0] = 1;
  uint8_t ctx2[HOP_CTX_LEN] = {0}; ctx2[0] = 2;
  uint8_t prev[AUTH_PATH_LEN] = {0};
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx1, prev, out1);
  chainStep(secret, ctx2, prev, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}

TEST(RouteMac, ChainStep_ChangesWhenPrevMacChanges) {
  uint8_t secret[32] = {0};
  uint8_t ctx[HOP_CTX_LEN] = {0};
  uint8_t p1[AUTH_PATH_LEN] = {0};
  uint8_t p2[AUTH_PATH_LEN] = {0}; p2[0] = 1;
  uint8_t out1[AUTH_PATH_LEN], out2[AUTH_PATH_LEN];
  chainStep(secret, ctx, p1, out1);
  chainStep(secret, ctx, p2, out2);
  ASSERT_NE(0, memcmp(out1, out2, AUTH_PATH_LEN));
}
