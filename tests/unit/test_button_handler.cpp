#include <gtest/gtest.h>
#include "app/ButtonHandler.h"
#include "hardware/input/Button.h"
#include "hardware/output/Led.h"
#include "mesh/Mesh.h"
#include "time_mock.h"
#include "esp_timer.h"
#include "Arduino.h"

using lattice::hardware::Button;
using lattice::hardware::Led;
using lattice::mesh::Mesh;

namespace {

constexpr uint8_t kConfigButtonPin = 27;
constexpr uint8_t kResetButtonPin = 26;
constexpr uint8_t kGreenLedPin = 25;
// NOTE: 24 is NOT a valid GpioOutput pin (GpioOutput::isValidOutputPin's mask
// excludes it) — Led::init() would silently fail, leaving the LED
// uninitialized, and the first pulse() call would then hit its
// not-initialized branch and escalate through lattice::err::fail() into a
// HARDWARE_FAILURE restart (thrown as ErrorCore::restartDevice under
// UNIT_TEST). 23 is a valid output pin and distinct from every other pin
// used in this fixture.
constexpr uint8_t kRedLedPin = 23;

// ButtonHandler::HOLD_MS / the reset flow's confirm window, mirrored here so
// the tests' timeline math is self-documenting (ButtonHandler.h keeps its
// own private copies; these are not the same symbols, just equal values).
constexpr uint64_t kHoldMs = 5000;
constexpr uint64_t kConfirmWindowMs = 3000;

} // namespace

class ButtonHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    resetMockDigitalPins();
    resetMockDigitalWriteCallCount();
    ESP._restartRequested = false;

    configBtn = std::make_unique<Button>(kConfigButtonPin);
    resetBtn = std::make_unique<Button>(kResetButtonPin);
    greenLed = std::make_unique<Led>(kGreenLedPin);
    redLed = std::make_unique<Led>(kRedLedPin);

    configBtn->init();
    resetBtn->init();
    greenLed->init();
    redLed->init();

    devMasterFlag = false;

    quiesceButtonHandlerStatics();
  }

  // ButtonHandler::tickConfig/tickReset keep their hold-tracking state
  // (wasPressed/holdStart, and tickReset's confirmPending/confirmDeadline)
  // as FUNCTION-LOCAL statics — by design, matching the pre-refactor code —
  // so that state persists across every TEST_F in this binary, not just
  // this fixture's lifetime. A prior test's leftover confirmPending==true
  // plus a stale confirmDeadline can otherwise make an unrelated later
  // test's very first hold-fire land in the confirmed-wipe branch (which
  // calls esp_restart() via a blocking pumpLedsUntilIdle() loop the host
  // mock clock can never satisfy — see the file-level comment further
  // down). Force a known baseline here: jump the mock clock far enough
  // ahead that "now" is guaranteed to exceed any stale confirmDeadline a
  // prior test could have set (every test's deadlines are computed
  // relative to ITS OWN resetMillis()-to-0 baseline and never advance more
  // than a few tens of seconds past it), then tick once with both buttons
  // unpressed (fresh Button objects read as unpressed on their very first
  // sample — see Button.cpp), which unconditionally resets both
  // functions' wasPressed to false and, since !isPressed() holds, clears a
  // stale confirmPending via the ordinary timeout path — never the
  // confirmed-wipe path, which requires isPressed()==true. Finally reset
  // the clock back to 0 for the test itself.
  void quiesceButtonHandlerStatics() {
    advanceMillis(1000000);
    tick(true);
    resetMillis();
    resetMockDigitalPins();
  }

  void TearDown() override {
    configBtn.reset();
    resetBtn.reset();
    greenLed.reset();
    redLed.reset();
  }

  // Button::isPressed() debounces over DEBOUNCE_READS(3) samples taken at
  // most once per DEBOUNCE_DELAY_MS(5ms) (see Button.cpp) — a single call
  // right after flipping the pin is NOT enough to read as pressed. Warm the
  // debounce history up front (mirrors tests/unit/test_button.cpp's own
  // sequencing) so that from the caller's very next tick() onward,
  // isPressed() deterministically reads the new pin state — keeping the
  // hold-duration math below exact instead of debounce-fuzzy.
  void pressButton(Button& btn, uint8_t pin) {
    setMockDigitalRead(pin, HIGH);
    // Leading advance guarantees >=DEBOUNCE_DELAY_MS has elapsed since this
    // Button's last actual sample (whatever it was, possibly 0ms ago if the
    // previous call was on the same tick) so the very next isPressed() call
    // is always a fresh sample rather than a same-window cache hit.
    advanceMillis(5);
    btn.isPressed();
    advanceMillis(5);
    btn.isPressed();
    advanceMillis(5);
    btn.isPressed(); // 3rd consecutive fresh HIGH sample -> debounced-pressed
    advanceMillis(5);
  }

  void releaseButton(Button& btn, uint8_t pin) {
    setMockDigitalRead(pin, LOW);
    advanceMillis(5); // see pressButton() — guarantees a fresh sample
    btn.isPressed();  // a single fresh LOW sample immediately breaks the streak
    advanceMillis(5);
  }

  void tick(bool isDevMode = true) {
    lattice::app::ButtonHandler::tick(*configBtn, *resetBtn, mesh, *greenLed, *redLed, isDevMode,
                                      devMasterFlag);
  }

  // Drains a Led's in-flight pulse() pattern to idle using synthetic,
  // caller-chosen timestamps fed directly to Led::update(uint64_t) — NOT the
  // shared mock millis() clock. ButtonHandler's tickReset only pumps
  // Led::update() via pumpLedsUntilIdle() on the (production-restart)
  // confirmed-wipe path, which this test suite deliberately never drives
  // (see the file-level comment below), so nothing else advances these LEDs.
  // Draining this way lets a test observe "did tickReset call pulse() again"
  // via a clean idle->busy transition, independent of and without
  // perturbing the resetBtn hold-timing clock under test.
  void drainLed(Led& led, uint64_t startMs) {
    uint64_t t = startMs;
    for (int i = 0; i < 8; ++i) { // comfortably more than the 5 flips a
                                  // pulse(3, 100, 100) pattern needs
      t += 100;
      led.update(t);
    }
    ASSERT_FALSE(led.isBusy()) << "drainLed: pattern did not finish draining";
  }

  Mesh mesh; // Local instance (default isMaster == false) — ButtonHandler::tick()
             // wants a Mesh&, and Mesh::getInstance() returns a Mesh* that is
             // null until some Mesh object has been constructed anyway, so a
             // local instance is both simpler and avoids relying on
             // process-wide singleton state leaking across tests.
  std::unique_ptr<Button> configBtn;
  std::unique_ptr<Button> resetBtn;
  std::unique_ptr<Led> greenLed;
  std::unique_ptr<Led> redLed;
  bool devMasterFlag;
};

// --- tickConfig (dev mode only — the non-dev branch calls esp_restart() via
// a blocking pumpLedsUntilIdle() loop that the host mock clock, which only
// advances on explicit advanceMillis() calls, can never satisfy; see the
// reset-button section below for the full explanation) ---

TEST_F(ButtonHandlerTest, ConfigButtonDoesNotFireBeforeHoldThreshold) {
  pressButton(*configBtn, kConfigButtonPin);
  tick(); // arms: wasPressed=true, holdStart=now

  advanceMillis(kHoldMs - 1);
  tick(); // one ms short of HOLD_MS -> must not fire

  EXPECT_FALSE(devMasterFlag);
  EXPECT_FALSE(mesh.getIsMaster());
}

TEST_F(ButtonHandlerTest, ConfigButtonFiresExactlyAtHoldThreshold) {
  pressButton(*configBtn, kConfigButtonPin);
  tick(); // arms

  advanceMillis(kHoldMs);
  tick(); // now - holdStart == HOLD_MS -> fires

  EXPECT_TRUE(devMasterFlag);
  EXPECT_TRUE(mesh.getIsMaster());
}

TEST_F(ButtonHandlerTest, ConfigButtonDoesNotRefireAfterRelease) {
  pressButton(*configBtn, kConfigButtonPin);
  tick();
  advanceMillis(kHoldMs);
  tick(); // fires once
  ASSERT_TRUE(devMasterFlag);

  releaseButton(*configBtn, kConfigButtonPin);
  advanceMillis(kHoldMs * 3);
  for (int i = 0; i < 5; ++i) {
    tick();
    advanceMillis(1000);
  }

  // Released, never re-pressed -> no further toggles.
  EXPECT_TRUE(devMasterFlag);
  EXPECT_TRUE(mesh.getIsMaster());
}

// --- tickReset ---
//
// tickReset's two "fire" branches (the confirmed-wipe path, and tickConfig's
// non-dev-mode counterpart) both end by calling pumpLedsUntilIdle() before
// esp_restart(). That helper spin-loops on esp_timer_get_time() without ever
// calling advanceMillis() itself, and vTaskDelay() is a no-op on host
// (tests/mocks/freertos/task.h) — so on this test harness that loop never
// terminates. Every test below is therefore deliberately written to never
// reach the confirmed-wipe branch (i.e. never holds a second time within the
// 3s confirm window); the "confirm succeeds" transition itself is exercised
// by reaching the arm branch a second time (see
// ResetArmsAgainAfterAProperTimeout below), stopping short of the actual
// clearAll()/esp_restart() call.

TEST_F(ButtonHandlerTest, ResetButtonFirstHoldArmsConfirmWindow) {
  pressButton(*resetBtn, kResetButtonPin);
  tick(false); // arm the hold counter
  ASSERT_FALSE(redLed->isBusy());

  advanceMillis(kHoldMs);
  tick(false); // fires -> arms the confirm window

  EXPECT_TRUE(redLed->isBusy()) << "arming the confirm window must pulse the red LED";
  EXPECT_FALSE(ESP._restartRequested);
}

// Covers Step 4(a)/(b) from the task brief: hold once, release, wait past
// confirmDeadline -> the confirm window must clear, and a fresh hold must be
// able to arm a brand-new window (observed via the red LED pulsing again
// after having been fully drained to idle).
TEST_F(ButtonHandlerTest, ResetArmsAgainAfterAProperTimeout) {
  pressButton(*resetBtn, kResetButtonPin);
  tick(false);
  advanceMillis(kHoldMs);
  tick(false); // first arm, confirmDeadline = now + 3000
  ASSERT_TRUE(redLed->isBusy());
  drainLed(*redLed, millis());

  releaseButton(*resetBtn, kResetButtonPin);
  advanceMillis(kConfirmWindowMs + 1000); // well past confirmDeadline, button released
  tick(false); // observes !isPressed() && confirmPending && expired -> clears confirmPending

  EXPECT_FALSE(redLed->isBusy()) << "timeout must not itself pulse the LED";

  // A brand new hold should now arm a fresh window rather than silently
  // no-op'ing (which is what would happen if confirmPending were still
  // stuck true from before).
  pressButton(*resetBtn, kResetButtonPin);
  tick(false);
  advanceMillis(kHoldMs);
  tick(false);

  EXPECT_TRUE(redLed->isBusy()) << "confirmPending must have been cleared by the timeout, "
                                   "allowing a fresh hold to re-arm";
  EXPECT_FALSE(ESP._restartRequested);
}

// The critical regression case flagged by the task brief: the ORIGINAL
// tickReset's timeout check lived in the `else` branch of
// `if (btn.isPressed())`, so it only ever ran while the button was NOT
// pressed. detectHold() returning false conflates "not pressed" with
// "pressed but still under HOLD_MS" — a naive migration that checks the
// timeout whenever detectHold() returns false (dropping the !isPressed()
// gate) fires the timeout mid-hold, the instant confirmDeadline elapses,
// even though the button was never released. That prematurely clears
// confirmPending, so the eventual second hold-fire (which — since
// HOLD_MS(5000) > the confirm window(3000) — always lands after
// confirmDeadline has already passed) sees confirmPending == false and
// incorrectly re-arms a fresh window instead of silently no-op'ing like the
// original.
TEST_F(ButtonHandlerTest, ResetTimeoutDoesNotFireWhileButtonIsContinuouslyHeld) {
  pressButton(*resetBtn, kResetButtonPin);
  tick(false); // arm hold counter, holdStart1 = millis()
  advanceMillis(kHoldMs);
  tick(false); // first fire @ t=holdStart1+5000 -> arms confirm window,
               // confirmDeadline = now + 3000
  ASSERT_TRUE(redLed->isBusy());

  // Drain the arm-pulse to idle using synthetic timestamps, independent of
  // the shared mock clock the button hold below is measured against.
  drainLed(*redLed, millis());
  ASSERT_FALSE(redLed->isBusy());

  // Do NOT release the button. The very next tick (still at the same
  // instant) re-arms detectHold's internal hold counter for a second
  // continuous hold, pinning holdStart2 == the first fire's timestamp.
  tick(false);

  // Walk the mock clock forward in 1s steps, ticking every step, all while
  // the button stays continuously pressed. This crosses confirmDeadline
  // (holdStart2 + 3000) well before the second hold-fire threshold
  // (holdStart2 + HOLD_MS == holdStart2 + 5000), which is exactly the
  // window the brief's risk note calls out.
  for (int i = 0; i < int(kHoldMs / 1000) + 1; ++i) {
    advanceMillis(1000);
    tick(false);
  }

  // We are now past confirmDeadline but still short of (or just at) the
  // second hold-fire threshold, and the button has never been released.
  // With the bug, the timeout branch would have already fired mid-hold and
  // cleared confirmPending; with the fix (this test), it must not have.
  EXPECT_FALSE(redLed->isBusy())
      << "confirmPending must NOT have been cleared while the button was continuously held "
         "past confirmDeadline — a bogus re-arm would pulse the red LED again";
  EXPECT_FALSE(ESP._restartRequested);

  ASSERT_TRUE(resetBtn->isPressed()) << "test invariant: button was held throughout";
}
