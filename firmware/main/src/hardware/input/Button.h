#ifndef BUTTON_H
#define BUTTON_H

#include "GpioInput.h"

namespace lattice {
namespace hardware {

class Button : public GpioInput {
public:
  explicit Button(uint8_t pin);
  // Initialize the button GPIO (uses internal PULL-DOWN)
  bool init();
  // Returns true if the button is currently debounced-pressed (active HIGH).
  // Non-blocking (audit item T): internally samples the pin at most once per
  // DEBOUNCE_DELAY_MS and rolls the result into a small history bitfield;
  // callers are expected to poll this every loop() iteration (as
  // ButtonHandler does) rather than spin-wait on it.
  bool isPressed();
  // Returns true if held for the given ms. Still a blocking call by design —
  // "wait" implies synchronous blocking until the hold completes or the
  // button releases — and is NOT used anywhere in the non-blocking main
  // loop (ButtonHandler polls isPressed() + tracks hold duration itself).
  // Kept for API compatibility; internally now calls the non-blocking
  // isPressed() so it no longer pays the old 10ms-per-read debounce cost,
  // just the intentional poll-loop delay.
  bool waitForHold(uint32_t ms);

private:
  static constexpr uint8_t DEBOUNCE_READS = 3;    // consecutive positive samples required
  static constexpr uint8_t DEBOUNCE_DELAY_MS = 5; // sample period
  static constexpr uint8_t DEBOUNCE_HISTORY_MASK = (1u << DEBOUNCE_READS) - 1u; // last 3 samples

  uint32_t _lastPollMs = 0;
  uint8_t _history = 0;    // rolling bitfield of raw samples, newest in bit0
  bool _hasPolled = false; // true once the first sample has been taken
};

} // namespace hardware
} // namespace lattice

#endif
