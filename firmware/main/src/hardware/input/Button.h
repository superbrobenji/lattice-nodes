#ifndef BUTTON_H
#define BUTTON_H

#include "GpioInput.h"

namespace lattice {
namespace hardware {

class Button : public GpioInput {
public:
  explicit Button(uint8_t pin);
  // Initialize the button GPIO (uses internal PULL-DOWN)
  bool init() override;
  // Returns true if the button is currently pressed (active HIGH)
  bool isPressed();
  // Returns true if held for the given ms (blocking call)
  bool waitForHold(uint32_t ms);

private:
  static constexpr uint8_t DEBOUNCE_READS = 3;
  static constexpr uint8_t DEBOUNCE_DELAY_MS = 5;
};

} // namespace hardware
} // namespace lattice

#endif
