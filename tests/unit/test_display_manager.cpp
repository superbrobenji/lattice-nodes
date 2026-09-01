#include <gtest/gtest.h>
#include "app/DisplayManager.h"
#include "hardware/output/SevenSegDisplay.h"
#include "time_mock.h"
#include "Arduino.h" // resetMockDigitalWriteCallCount / _mockDigitalWriteCallCount

using lattice::app::DisplayManager;
using lattice::hardware::SevenSegDisplay;

// NOTE: DisplayManager::tick() carries its change-detection state in
// function-local statics (matching the pre-existing lastToggleMs/dashVisible
// pattern), so state persists across calls within a single test body — which
// is exactly what these tests exercise. Each TEST_F below runs as its own
// process via gtest_discover_tests()'s per-test --gtest_filter invocation,
// so state does NOT leak between test cases.
class DisplayManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetMillis();
    resetMockDigitalWriteCallCount();
  }
};

// Item S: repeated tick() calls with an unchanged (enrolled, isMaster,
// nodeId) tuple must not re-issue the bit-banged display write.
TEST_F(DisplayManagerTest, RepeatedTickWithSameValueDoesNotRewriteDisplay) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, /*enrolled=*/true, /*isMaster=*/false, /*nodeId=*/3);
  int afterFirst = _mockDigitalWriteCallCount;
  EXPECT_GT(afterFirst, 0) << "first tick() with a real nodeId must draw something";

  DisplayManager::tick(disp, true, false, 3);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "unchanged value must not redraw";

  DisplayManager::tick(disp, true, false, 3);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "unchanged value must not redraw (again)";
}

TEST_F(DisplayManagerTest, NodeIdChangeTriggersRedraw) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, 3);
  int afterFirst = _mockDigitalWriteCallCount;

  DisplayManager::tick(disp, true, false, 7);
  EXPECT_GT(_mockDigitalWriteCallCount, afterFirst) << "nodeId change must redraw";
}

// isMaster can flip live at runtime (dev-mode button hold toggles
// mesh.setIsMaster()); the throttle must not swallow that even when nodeId
// is unchanged, since it changes which display method (show vs showWithDP)
// is used.
TEST_F(DisplayManagerTest, MasterFlagChangeTriggersRedrawEvenIfNodeIdSame) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, 3);
  int afterFirst = _mockDigitalWriteCallCount;

  DisplayManager::tick(disp, true, true, 3);
  EXPECT_GT(_mockDigitalWriteCallCount, afterFirst) << "isMaster change must redraw";
}

// The pre-enroll blink state keeps its own independent 500ms toggle timer
// (unchanged by item S) and must keep working exactly as before. Note:
// lastToggleMs and millis() both start at 0, so the very first call doesn't
// cross the 500ms threshold yet — that's pre-existing behavior, unrelated to
// item S, and preserved as-is.
TEST_F(DisplayManagerTest, PreEnrollBlinkTogglesOnlyEvery500ms) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, /*enrolled=*/false, false, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, 0);

  advanceMillis(500);
  DisplayManager::tick(disp, false, false, 0);
  int afterToggle = _mockDigitalWriteCallCount;
  EXPECT_GT(afterToggle, 0) << "500ms elapsed must toggle dash blink";

  // Still within the next 500ms window.
  advanceMillis(100);
  DisplayManager::tick(disp, false, false, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterToggle);

  advanceMillis(400);
  DisplayManager::tick(disp, false, false, 0);
  EXPECT_GT(_mockDigitalWriteCallCount, afterToggle) << "next 500ms window must toggle again";
}

// Re-entering the enrolled branch after a pre-enroll blink must redraw even
// if nodeId happens to match whatever was last drawn before enrollment.
TEST_F(DisplayManagerTest, RedrawsOnceEnrolledAfterPreEnrollBlink) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, 5); // enrolled, draws nodeId 5
  int afterEnrolledDraw = _mockDigitalWriteCallCount;
  EXPECT_GT(afterEnrolledDraw, 0);

  advanceMillis(500);                          // cross the blink toggle's 500ms threshold
  DisplayManager::tick(disp, false, false, 0); // drop out of enrolled -> blink path
  int afterBlink = _mockDigitalWriteCallCount;
  EXPECT_GT(afterBlink, afterEnrolledDraw);

  // Re-enroll with the SAME nodeId as before (5) — must still redraw since
  // the display currently shows a blink dash, not the number.
  DisplayManager::tick(disp, true, false, 5);
  EXPECT_GT(_mockDigitalWriteCallCount, afterBlink);
}

// Issue #118: masters bypass the hub's ID-assignment flow, so every real
// master's nodeId is 0. tick() used to check nodeId == 0 *before* isMaster,
// so the documented decimal-point master indicator never rendered on an
// actual master — it showed a plain "0", indistinguishable from an unset ID.
TEST_F(DisplayManagerTest, MasterWithNodeIdZeroRendersDecimalPoint) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, /*enrolled=*/true, /*isMaster=*/true, /*nodeId=*/0);
  EXPECT_GT(_mockDigitalWriteCallCount, 0) << "master must draw something";
  EXPECT_NE(disp.testLastSegments()[3] & 0x80, 0)
      << "master with nodeId 0 must light the decimal point (bit7 of the last digit)";
}
