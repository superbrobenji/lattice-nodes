#include <gtest/gtest.h>
#include "harness/NodeContext.h"
#include "harness/SimClock.h"
#include "EEPROM.h"
#include "esp_wifi_mock.h"

TEST(NodeContextSwap, IsolatesEepromAndMac) {
  sim::NodeContext a, b;
  a.mac[0] = 0xAA;
  b.mac[0] = 0xBB;

  sim::swapIn(a);
  EEPROM.write(0, 0x11);
  EXPECT_EQ(mockDeviceMac[0], 0xAA);
  sim::swapOut(a);

  sim::swapIn(b);
  EXPECT_EQ(EEPROM.read(0), 0xFF) << "node B must not see node A's EEPROM";
  EXPECT_EQ(mockDeviceMac[0], 0xBB);
  EEPROM.write(0, 0x22);
  sim::swapOut(b);

  sim::swapIn(a);
  EXPECT_EQ(EEPROM.read(0), 0x11);
  sim::swapOut(a);
}

TEST(SimClockTest, AdvancesMillis) {
  sim::SimClock clock;
  uint32_t t0 = clock.now();
  clock.advance(250);
  EXPECT_EQ(clock.now(), t0 + 250);
  EXPECT_EQ(millis(), clock.now());
}
