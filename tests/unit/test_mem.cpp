#include <gtest/gtest.h>
#include "src/network/mem.h"

using lattice::mem::is_zero;

TEST(MemIsZero, AllZeroBytesReturnsTrue) {
  const uint8_t buf[6] = {0, 0, 0, 0, 0, 0};
  EXPECT_TRUE(is_zero(buf, 6));
}

TEST(MemIsZero, AnyNonZeroByteReturnsFalse) {
  const uint8_t buf[6] = {0, 0, 0, 1, 0, 0};
  EXPECT_FALSE(is_zero(buf, 6));
}

TEST(MemIsZero, NonZeroFirstByteReturnsFalse) {
  const uint8_t buf[6] = {1, 0, 0, 0, 0, 0};
  EXPECT_FALSE(is_zero(buf, 6));
}

TEST(MemIsZero, NonZeroLastByteReturnsFalse) {
  const uint8_t buf[6] = {0, 0, 0, 0, 0, 1};
  EXPECT_FALSE(is_zero(buf, 6));
}

TEST(MemIsZero, ZeroLengthReturnsTrue) {
  const uint8_t buf[1] = {0xFF}; // untouched — n=0 must not read it
  EXPECT_TRUE(is_zero(buf, 0));
}

TEST(MemIsZero, ThirtyTwoByteAllZero) {
  uint8_t buf[32] = {};
  EXPECT_TRUE(is_zero(buf, 32));
}

TEST(MemIsZero, ThirtyTwoByteOneNonZero) {
  uint8_t buf[32] = {};
  buf[31] = 0x01;
  EXPECT_FALSE(is_zero(buf, 32));
}
