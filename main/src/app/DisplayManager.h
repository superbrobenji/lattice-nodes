#pragma once
#include <cstdint>
#include <Arduino.h>
#include "src/hardware/output/SevenSegDisplay.h"

namespace lattice {
namespace app {

struct DisplayManager {
  static void tick(lattice::hardware::SevenSegDisplay& display,
                   bool enrolled, bool isMaster, uint8_t nodeId) {
    static uint32_t lastToggleMs = 0;
    static bool dashVisible = false;

    if (!enrolled) {
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
    } else if (nodeId == 0) {
      display.show(0, false);
    } else if (isMaster) {
      display.showWithDP(static_cast<int>(nodeId), false);
    } else {
      display.show(static_cast<int>(nodeId), false);
    }
  }
};

} // namespace app
} // namespace lattice
