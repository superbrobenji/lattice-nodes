// test_smoke.cpp — Task 1 smoke test: verifies the build system and mock layer work
#include <gtest/gtest.h>

// Basic smoke test: the test harness itself works
TEST(Smoke, TrueIsTrue) {
  EXPECT_TRUE(true);
}

// Verify mock clock is controllable
#include "time_mock.h"
TEST(Smoke, MockClockWorks) {
  resetMillis();
  EXPECT_EQ(millis(), 0u);
  advanceMillis(1000);
  EXPECT_EQ(millis(), 1000u);
  resetMillis();
}

// Verify EEPROM mock is functional
#include "EEPROM.h"
TEST(Smoke, EEPROMMockWorks) {
  EEPROM.reset();
  EEPROM.write(0, 0xAB);
  EXPECT_EQ(EEPROM.read(0), 0xAB);
  EEPROM.commit();
  EXPECT_EQ(EEPROM.commitCount(), 1);
}
