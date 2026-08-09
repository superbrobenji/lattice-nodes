#include "Button.h"
#include <cstdint>
#include <esp_timer.h>
#include <driver/gpio.h>

namespace lattice {
namespace hardware {

Button::Button(uint8_t pin) : GpioInput(pin) {}

bool Button::init() {
  // Finding 9: delegates to the shared base validation, matching
  // Pir::init()'s pattern of calling GpioInput::init() explicitly — Button
  // has no extra per-init state to reset on success, so there's nothing
  // else to do here. Phase I Task 7 (RR)'s note still applies: no per-init
  // pinMode() call — this pin is part of main.cpp's bundled input-group
  // gpio_config_t (pull-DOWN, matching the prior INPUT_PULLDOWN behavior —
  // the line is LOW unless actively driven HIGH), applied once at boot.
  return GpioInput::init();
}

bool Button::isPressed() {
  // Rolling-vote debounce (audit item T): take at most one raw sample every
  // DEBOUNCE_DELAY_MS, shifting it into a small history bitfield. Callers
  // poll this repeatedly (e.g. once per main loop) instead of blocking here
  // — the old implementation blocked for DEBOUNCE_DELAY_MS * (DEBOUNCE_READS
  // - 1) = 10ms per call via delay().
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  if (!_hasPolled || (now - _lastPollMs) >= DEBOUNCE_DELAY_MS) {
    _hasPolled = true;
    _lastPollMs = now;
    bool raw = (gpio_get_level(static_cast<gpio_num_t>(_pin)) == 1);
    _history = static_cast<uint8_t>((_history << 1) | (raw ? 1u : 0u));
  }
  // Pressed once the DEBOUNCE_READS most-recent samples (~DEBOUNCE_READS *
  // DEBOUNCE_DELAY_MS <= 20ms window) are all HIGH.
  return (_history & DEBOUNCE_HISTORY_MASK) == DEBOUNCE_HISTORY_MASK;
}

} // namespace hardware
} // namespace lattice
