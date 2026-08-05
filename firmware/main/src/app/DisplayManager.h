#pragma once
#include <cstdint>
#include <Arduino.h>
#include "src/hardware/output/SevenSegDisplay.h"

namespace lattice {
namespace app {

struct DisplayManager {
  static void tick(lattice::hardware::SevenSegDisplay& display, bool enrolled, bool isMaster,
                   uint8_t nodeId) {
    static uint32_t lastToggleMs = 0;
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
      if (millis() - lastToggleMs >= 500) {
        lastToggleMs = static_cast<uint32_t>(millis());
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

    if (nodeId == 0) {
      display.show(0, false);
    } else if (isMaster) {
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
