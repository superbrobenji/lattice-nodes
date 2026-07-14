#include <gtest/gtest.h>
#include "Arduino.h"
#include "serial_mock.h"
#include "src/error/Error.h"

TEST(SerialMock, RxQueueRoundTrip) {
  Serial.reset();
  uint8_t bytes[3] = {0xAA, 0xBB, 0xCC};
  Serial.injectRx(bytes, 3);
  ASSERT_EQ(Serial.available(), 3);
  EXPECT_EQ(Serial.read(), 0xAA);
  EXPECT_EQ(Serial.read(), 0xBB);
  EXPECT_EQ(Serial.read(), 0xCC);
  EXPECT_EQ(Serial.available(), 0);
  EXPECT_EQ(Serial.read(), -1);
}

TEST(EspMock, RestartSetsFlag) {
  ESP._restartRequested = false;
  ESP.restart();
  EXPECT_TRUE(ESP._restartRequested);
}

TEST(ErrorHooks, FatalThrows) {
  EXPECT_THROW(
      lattice::err::fatal(lattice::core::ErrorTypeDigit::GENERIC,
                          lattice::core::ModuleDigit::CORE, 9, "boom"),
      lattice::err::FatalError);
}

TEST(ErrorHooks, FailIncrementsCounter) {
  int before = lattice_test_errFailCount;
  lattice::err::fail(lattice::utils::ErrorType::GENERIC, "soft");
  EXPECT_EQ(lattice_test_errFailCount, before + 1);
}
