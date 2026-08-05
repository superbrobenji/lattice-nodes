#include "Button.h"
#include <Arduino.h>
#include <cstdint>

namespace lattice {
namespace hardware {

Button::Button(uint8_t pin) : GpioInput(pin) {}

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
  // Rolling-vote debounce (audit item T): take at most one raw sample every
  // DEBOUNCE_DELAY_MS, shifting it into a small history bitfield. Callers
  // poll this repeatedly (e.g. once per main loop) instead of blocking here
  // — the old implementation blocked for DEBOUNCE_DELAY_MS * (DEBOUNCE_READS
  // - 1) = 10ms per call via delay().
  uint32_t now = static_cast<uint32_t>(millis());
  if (!_hasPolled || static_cast<uint32_t>(now - _lastPollMs) >= DEBOUNCE_DELAY_MS) {
    _hasPolled = true;
    _lastPollMs = now;
    bool raw = (digitalRead(_pin) == HIGH);
    _history = static_cast<uint8_t>((_history << 1) | (raw ? 1u : 0u));
  }
  // Pressed once the DEBOUNCE_READS most-recent samples (~DEBOUNCE_READS *
  // DEBOUNCE_DELAY_MS <= 20ms window) are all HIGH.
  return (_history & DEBOUNCE_HISTORY_MASK) == DEBOUNCE_HISTORY_MASK;
}

bool Button::waitForHold(uint32_t ms) {
  uint32_t start = millis();
  if (!isPressed())
    return false;
  while (isPressed()) {
    if (static_cast<uint32_t>(millis() - start) >= ms)
      return true;
    delay(10); // yield to RTOS; isPressed() itself is non-blocking (item T)
  }
  return false;
}

} // namespace hardware
} // namespace lattice
