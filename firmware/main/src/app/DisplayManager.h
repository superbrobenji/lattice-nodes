#pragma once
#include <cstdint>
#include <esp_timer.h>
#include "src/hardware/output/SevenSegDisplay.h"

namespace lattice {
namespace app {

struct DisplayManager {
  // Decimal-point treatment for the enrolled display state (issue #118). The
  // digit is always the node ID (0 for masters, which skip hub ID assignment);
  // the decimal point on the last digit encodes the role:
  //   OFF   — leaf: plain node ID.
  //   SOLID — primary master (own MAC == pin::MASTER_MAC).
  //   BLINK — secondary master (own MAC != pin::MASTER_MAC; DUAL_MASTER_MODE).
  enum class Dp : uint8_t { OFF, SOLID, BLINK };

  // Half-period shared by the pre-enroll dash blink and the secondary DP blink.
  static constexpr uint64_t BLINK_HALF_PERIOD_MS = 500;

  static Dp dpFor(bool isMaster, bool isPrimaryMaster) {
    if (!isMaster)
      return Dp::OFF;
    return isPrimaryMaster ? Dp::SOLID : Dp::BLINK;
  }

  // Whether the decimal point is lit at nowMs. BLINK derives its phase from
  // the clock (lit during even half-periods) rather than a toggle, so it needs
  // no extra state and the change detection in tick() can compare it directly.
  static bool dpLit(Dp dp, uint64_t nowMs) {
    if (dp == Dp::SOLID)
      return true;
    if (dp == Dp::BLINK)
      return ((nowMs / BLINK_HALF_PERIOD_MS) & 1ULL) == 0;
    return false;
  }

  // isPrimaryMaster is only meaningful when isMaster (main.cpp computes it as
  // isMaster && ownMac == pin::MASTER_MAC); a leaf ignores it.
  static void tick(lattice::hardware::SevenSegDisplay& display, bool enrolled, bool isMaster,
                   bool isPrimaryMaster, uint8_t nodeId) {
    // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
    // millis() -> esp_timer_get_time()/1000ULL swap.
    static uint64_t lastToggleMs = 0;
    static bool dashVisible = false;

    // Change-detection state (audit item S) — display.show()/showWithDP() only fire
    // when the rendered frame actually changes, instead of every tick(). _lastValue
    // sentinel of -1 (out of the int8 nodeId range) forces a draw on the first
    // enrolled tick and again whenever we re-enter the enrolled branch after a
    // pre-enroll blink (isEnrolled flips false->true).
    static int _lastValue = -1;
    static bool _lastDpLit = false;
    static bool _wasEnrolled = false;

    const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;

    if (!enrolled) {
      _wasEnrolled = false; // force a redraw once we become enrolled again
      if (nowMs - lastToggleMs >= BLINK_HALF_PERIOD_MS) {
        lastToggleMs = nowMs;
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

    // The role decides the indicator, not nodeId: masters bypass hub ID
    // assignment, so a real master's nodeId is 0 (issue #118) — checking
    // nodeId == 0 first used to swallow the decimal point on every master.
    const int value = static_cast<int>(nodeId);
    const bool lit = dpLit(dpFor(isMaster, isPrimaryMaster), nowMs);
    const bool changed = !_wasEnrolled || value != _lastValue || lit != _lastDpLit;
    if (!changed) {
      return;
    }

    if (lit) {
      display.showWithDP(value, false);
    } else {
      display.show(value, false);
    }

    _lastValue = value;
    _lastDpLit = lit;
    _wasEnrolled = true;
  }
};

} // namespace app
} // namespace lattice
