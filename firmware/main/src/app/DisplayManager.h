#pragma once
#include <cstdint>
#include <esp_timer.h>
#include "src/hardware/output/SevenSegDisplay.h"

namespace lattice {
namespace app {

struct DisplayManager {
  static void tick(lattice::hardware::SevenSegDisplay& display, bool enrolled, bool isMaster,
                   uint8_t nodeId) {
    // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
    // millis() -> esp_timer_get_time()/1000ULL swap.
    static uint64_t lastToggleMs = 0;
    static bool dashVisible = false;

    // Change-detection state (audit item S) — display.show()/showWithDP() only fire
    // when the rendered value actually changes, instead of every tick(). _lastValue
    // sentinel of -1 (out of the int8 nodeId range) forces a draw on the first
    // enrolled tick and again whenever we re-enter the enrolled branch after a
    // pre-enroll blink (isEnrolled flips false->true).
    static int _lastValue = -1;
    static uint8_t _lastNodeId = 0;
    static bool _lastIsMaster = false;
    static bool _wasEnrolled = false;

    if (!enrolled) {
      _wasEnrolled = false; // force a redraw once we become enrolled again
      if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL - lastToggleMs >= 500) {
        lastToggleMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        dashVisible = !dashVisible;
        if (dashVisible) {
          static const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
          display.setSegments(dashes);
        } else {
          display.clear();
        }
      }
      return;
    }

    const int value = static_cast<int>(nodeId);
    const bool changed =
        !_wasEnrolled || value != _lastValue || nodeId != _lastNodeId || isMaster != _lastIsMaster;
    if (!changed) {
      return;
    }

    // isMaster decides the indicator, not nodeId: masters bypass hub ID
    // assignment, so a real master's nodeId is 0 (issue #118) — checking
    // nodeId == 0 first used to swallow the decimal point on every master.
    if (isMaster) {
      display.showWithDP(value, false);
    } else {
      display.show(value, false);
    }

    _lastValue = value;
    _lastNodeId = nodeId;
    _lastIsMaster = isMaster;
    _wasEnrolled = true;
  }
};

} // namespace app
} // namespace lattice
