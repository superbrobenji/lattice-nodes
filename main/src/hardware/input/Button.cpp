#include "Button.h"
#include <Arduino.h>
#include <cstdint>

namespace planetopia {
namespace hardware {

Button::Button(uint8_t pin)
  : GpioInput(pin) {}

bool Button::init() {
  if (!isValidInputPin(_pin)) {
    return false;
  }
  // Use the ESP32 internal pull-down resistor so the line is LOW unless actively driven HIGH.
#if defined(ESP32)
  pinMode(_pin, INPUT_PULLDOWN);
#else
  // Fallback – not all MCUs support internal pull-downs. External resistor required.
  pinMode(_pin, INPUT);
#endif
  _initialized = true;
  return true;
}

bool Button::isPressed() {
  // Active HIGH (assumes INPUT_PULLDOWN or external pull-down)
  return digitalRead(_pin) == HIGH;
}

bool Button::waitForHold(uint32_t ms) {
  uint32_t start = millis();
  if (!isPressed()) return false;
  while (isPressed()) {
    if (static_cast<uint32_t>(millis() - start) >= ms) return true;
    delay(10);  // debounce, yield to RTOS
  }
  return false;
}

}  // namespace hardware
}  // namespace planetopia
