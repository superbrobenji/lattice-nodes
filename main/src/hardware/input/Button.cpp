#include "Button.h"
#include <Arduino.h>

namespace planetopia {
namespace hardware {

Button::Button(uint8_t pin) : GpioInput(pin) {}

bool Button::isPressed() {
    // Active LOW (assumes INPUT_PULLUP)
    return digitalRead(_pin) == LOW;
}

bool Button::waitForHold(unsigned long ms) {
    unsigned long start = millis();
    if (!isPressed()) return false;
    while (isPressed()) {
        if (millis() - start >= ms) return true;
        delay(10); // debounce, yield to RTOS
    }
    return false;
}

}  // namespace hardware
}  // namespace planetopia
