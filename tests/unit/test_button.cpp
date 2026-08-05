#include <gtest/gtest.h>
#include "hardware/input/Button.h"
#include "time_mock.h"
#include "Arduino.h" // setMockDigitalRead / resetMockDigitalPins / HIGH / LOW

using lattice::hardware::Button;

// Pin 27 is a valid input pin per GpioInput::isValidInputPin (matches
// tests/unit/test_pir_adapter.cpp convention).
static constexpr uint8_t kTestPin = 27;

class ButtonTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    resetMockDigitalPins();
  }
};

// Item T: isPressed() must not block. With delay() a no-op under UNIT_TEST,
// a call that still internally blocked via delay() would return instantly
// regardless — so the real proof-of-non-blocking is that isPressed() only
// takes a fresh sample once per DEBOUNCE_DELAY_MS (5ms) of *simulated*
// millis(), not on every call. We drive that via advanceMillis() rather than
// wall-clock time.
TEST_F(ButtonTest, NotPressedWhenPinLow) {
  Button btn(kTestPin);
  btn.init();
  // Pin stays LOW (default). Poll several times, advancing the mock clock
  // past the debounce window each time.
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(btn.isPressed());
    advanceMillis(5);
  }
}

TEST_F(ButtonTest, RequiresConsecutiveHighSamplesBeforeReportingPressed) {
  Button btn(kTestPin);
  btn.init();
  setMockDigitalRead(kTestPin, HIGH);

  // First sample taken immediately (not yet debounced: only 1 positive sample).
  EXPECT_FALSE(btn.isPressed());

  // A second poll before 5ms has elapsed must NOT take a new sample — still
  // only 1 positive sample banked, so still not pressed.
  EXPECT_FALSE(btn.isPressed());

  advanceMillis(5); // 2nd sample window
  EXPECT_FALSE(btn.isPressed());

  advanceMillis(5); // 3rd sample window -> 3 consecutive HIGH samples
  EXPECT_TRUE(btn.isPressed());
}

TEST_F(ButtonTest, ReleaseResetsTheDebounceStreak) {
  Button btn(kTestPin);
  btn.init();
  setMockDigitalRead(kTestPin, HIGH);

  btn.isPressed();
  advanceMillis(5);
  btn.isPressed();
  advanceMillis(5);
  ASSERT_TRUE(btn.isPressed());

  // Pin goes LOW — next sampled poll should immediately un-latch (a single
  // LOW sample breaks the "consecutive" requirement).
  setMockDigitalRead(kTestPin, LOW);
  advanceMillis(5);
  EXPECT_FALSE(btn.isPressed());
}

// Sanity: a call between two mock-clock advances that lands inside the same
// 5ms sample window returns the same cached vote (proves isPressed() isn't
// blocking-and-resampling every call, which was the pre-item-T behavior).
TEST_F(ButtonTest, DoesNotResampleWithinDebounceWindow) {
  Button btn(kTestPin);
  btn.init();
  setMockDigitalRead(kTestPin, HIGH);

  btn.isPressed();
  advanceMillis(5);
  btn.isPressed();
  advanceMillis(5);
  ASSERT_TRUE(btn.isPressed()); // latched pressed

  // Flip the pin LOW but do NOT advance the mock clock — within the same
  // debounce window the cached (pressed) vote must still hold.
  setMockDigitalRead(kTestPin, LOW);
  EXPECT_TRUE(btn.isPressed());
}
