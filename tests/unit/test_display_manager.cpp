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

  // A whole-binary run (./test_display_manager with no --gtest_filter) shares
  // tick()'s function-local statics across tests. Dropping out of the enrolled
  // branch first guarantees the next enrolled tick redraws regardless of what a
  // previous test left behind, so the #118 tests below hold in either mode.
  static void freshEnrolledState(SevenSegDisplay& disp) {
    DisplayManager::tick(disp, /*enrolled=*/false, false, false, 0);
    resetMockDigitalWriteCallCount();
  }
};

// Item S: repeated tick() calls with an unchanged (enrolled, isMaster,
// nodeId) tuple must not re-issue the bit-banged display write.
TEST_F(DisplayManagerTest, RepeatedTickWithSameValueDoesNotRewriteDisplay) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, /*enrolled=*/true, /*isMaster=*/false, /*isPrimaryMaster=*/false,
                       /*nodeId=*/3);
  int afterFirst = _mockDigitalWriteCallCount;
  EXPECT_GT(afterFirst, 0) << "first tick() with a real nodeId must draw something";

  DisplayManager::tick(disp, true, false, false, 3);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "unchanged value must not redraw";

  DisplayManager::tick(disp, true, false, false, 3);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "unchanged value must not redraw (again)";
}

TEST_F(DisplayManagerTest, NodeIdChangeTriggersRedraw) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, false, 3);
  int afterFirst = _mockDigitalWriteCallCount;

  DisplayManager::tick(disp, true, false, false, 7);
  EXPECT_GT(_mockDigitalWriteCallCount, afterFirst) << "nodeId change must redraw";
}

// isMaster can flip live at runtime (dev-mode button hold toggles
// mesh.setIsMaster()); the throttle must not swallow that even when nodeId
// is unchanged, since it changes which display method (show vs showWithDP)
// is used.
TEST_F(DisplayManagerTest, MasterFlagChangeTriggersRedrawEvenIfNodeIdSame) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, false, 3);
  int afterFirst = _mockDigitalWriteCallCount;

  DisplayManager::tick(disp, true, true, true, 3);
  EXPECT_GT(_mockDigitalWriteCallCount, afterFirst) << "isMaster change must redraw";
}

// The pre-enroll blink state keeps its own independent 500ms toggle timer
// (unchanged by item S) and must keep working exactly as before. Note:
// lastToggleMs and millis() both start at 0, so the very first call doesn't
// cross the 500ms threshold yet — that's pre-existing behavior, unrelated to
// item S, and preserved as-is.
TEST_F(DisplayManagerTest, PreEnrollBlinkTogglesOnlyEvery500ms) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, /*enrolled=*/false, false, false, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, 0);

  advanceMillis(500);
  DisplayManager::tick(disp, false, false, false, 0);
  int afterToggle = _mockDigitalWriteCallCount;
  EXPECT_GT(afterToggle, 0) << "500ms elapsed must toggle dash blink";

  // Still within the next 500ms window.
  advanceMillis(100);
  DisplayManager::tick(disp, false, false, false, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterToggle);

  advanceMillis(400);
  DisplayManager::tick(disp, false, false, false, 0);
  EXPECT_GT(_mockDigitalWriteCallCount, afterToggle) << "next 500ms window must toggle again";
}

// Re-entering the enrolled branch after a pre-enroll blink must redraw even
// if nodeId happens to match whatever was last drawn before enrollment.
TEST_F(DisplayManagerTest, RedrawsOnceEnrolledAfterPreEnrollBlink) {
  SevenSegDisplay disp(4, 5);

  DisplayManager::tick(disp, true, false, false, 5); // enrolled, draws nodeId 5
  int afterEnrolledDraw = _mockDigitalWriteCallCount;
  EXPECT_GT(afterEnrolledDraw, 0);

  advanceMillis(500);                                 // cross the blink toggle's 500ms threshold
  DisplayManager::tick(disp, false, false, false, 0); // drop out of enrolled -> blink path
  int afterBlink = _mockDigitalWriteCallCount;
  EXPECT_GT(afterBlink, afterEnrolledDraw);

  // Re-enroll with the SAME nodeId as before (5) — must still redraw since
  // the display currently shows a blink dash, not the number.
  DisplayManager::tick(disp, true, false, false, 5);
  EXPECT_GT(_mockDigitalWriteCallCount, afterBlink);
}

// Issue #118: masters bypass the hub's ID-assignment flow, so every real
// master's nodeId is 0. tick() used to check nodeId == 0 *before* isMaster,
// so the documented decimal-point master indicator never rendered on an
// actual master — it showed a plain "0", indistinguishable from an unset ID.
TEST_F(DisplayManagerTest, MasterWithNodeIdZeroRendersDecimalPoint) {
  SevenSegDisplay disp(4, 5);

  freshEnrolledState(disp);
  DisplayManager::tick(disp, /*enrolled=*/true, /*isMaster=*/true, /*isPrimaryMaster=*/true,
                       /*nodeId=*/0);
  EXPECT_GT(_mockDigitalWriteCallCount, 0) << "master must draw something";
  EXPECT_NE(disp.testLastSegments()[3] & 0x80, 0)
      << "master with nodeId 0 must light the decimal point (bit7 of the last digit)";
}

// --- Primary vs. secondary master (issue #118) ---
// main.cpp derives isPrimaryMaster locally: isMaster && ownMac == pin::MASTER_MAC.
// Primary = solid decimal point (the documented master indicator); secondary =
// the same digit with the decimal point blinking at the pre-enroll dash cadence.

TEST_F(DisplayManagerTest, PrimaryMasterHoldsDecimalPointSolidAcrossBlinkPeriods) {
  SevenSegDisplay disp(4, 5);

  freshEnrolledState(disp);
  DisplayManager::tick(disp, true, /*isMaster=*/true, /*isPrimaryMaster=*/true, 0);
  int afterFirst = _mockDigitalWriteCallCount;
  EXPECT_NE(disp.testLastSegments()[3] & 0x80, 0);

  advanceMillis(500);
  DisplayManager::tick(disp, true, true, true, 0);
  advanceMillis(500);
  DisplayManager::tick(disp, true, true, true, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "solid DP must not redraw as time passes";
  EXPECT_NE(disp.testLastSegments()[3] & 0x80, 0) << "solid DP must stay lit";
}

TEST_F(DisplayManagerTest, SecondaryMasterBlinksDecimalPointEvery500ms) {
  SevenSegDisplay disp(4, 5);

  freshEnrolledState(disp);
  DisplayManager::tick(disp, true, /*isMaster=*/true, /*isPrimaryMaster=*/false, 0);
  const bool litAtStart = (disp.testLastSegments()[3] & 0x80) != 0;
  const uint8_t digitAtStart = disp.testLastSegments()[3] & 0x7F;
  int afterFirst = _mockDigitalWriteCallCount;
  EXPECT_GT(afterFirst, 0);

  advanceMillis(100);
  DisplayManager::tick(disp, true, true, false, 0);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "inside a half-period: no redraw";

  advanceMillis(400);
  DisplayManager::tick(disp, true, true, false, 0);
  EXPECT_GT(_mockDigitalWriteCallCount, afterFirst) << "DP must toggle after 500ms";
  EXPECT_NE((disp.testLastSegments()[3] & 0x80) != 0, litAtStart) << "DP phase must flip";
  EXPECT_EQ(disp.testLastSegments()[3] & 0x7F, digitAtStart) << "digit itself must not change";
  int afterToggle = _mockDigitalWriteCallCount;

  advanceMillis(500);
  DisplayManager::tick(disp, true, true, false, 0);
  EXPECT_GT(_mockDigitalWriteCallCount, afterToggle) << "and toggle again after another 500ms";
  EXPECT_EQ((disp.testLastSegments()[3] & 0x80) != 0, litAtStart) << "back to initial phase";
}

TEST_F(DisplayManagerTest, LeafNeverLightsDecimalPointRegardlessOfPrimaryFlag) {
  SevenSegDisplay disp(4, 5);

  freshEnrolledState(disp);
  // isPrimaryMaster is only meaningful when isMaster; a leaf must ignore it.
  DisplayManager::tick(disp, true, /*isMaster=*/false, /*isPrimaryMaster=*/true, 3);
  int afterFirst = _mockDigitalWriteCallCount;
  EXPECT_EQ(disp.testLastSegments()[3] & 0x80, 0);

  advanceMillis(500);
  DisplayManager::tick(disp, true, false, true, 3);
  EXPECT_EQ(_mockDigitalWriteCallCount, afterFirst) << "leaf: no blink, no redraw";
  EXPECT_EQ(disp.testLastSegments()[3] & 0x80, 0);
}
