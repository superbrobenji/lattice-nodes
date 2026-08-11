#include "Led.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <cstdio>
#include <driver/gpio.h>
#include <esp_timer.h>

namespace lattice {
namespace hardware {

using namespace lattice::utils;

// Static member init
Led* Led::_systemErrorLed = nullptr;

// CONSTRUCTOR — now forwards pin to base GpioOutput!
Led::Led(uint8_t pin) : GpioOutput(pin), _isOn(false) {}

Led::~Led() {
  if (_initialized) {
    gpio_set_level(static_cast<gpio_num_t>(_pin), 0);
    (void)gpio_set_direction(static_cast<gpio_num_t>(_pin), GPIO_MODE_INPUT);
  }
}

// isValidPin is now inherited from GpioOutput

bool Led::init() {
  // Use GpioOutput::init() for validation and pinMode
  if (!GpioOutput::init()) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::CONFIG, lattice::core::ModuleDigit::HW, 1,
                         "Led: Invalid pin number");
    }
    LATTICE_LOGF("Led", LogLevel::LOG_ERROR, "ERROR: Invalid pin number for LED: %u",
                 (unsigned)_pin);
    return false;
  }
  gpio_set_level(static_cast<gpio_num_t>(_pin), 0);
  _isOn = false;
  LATTICE_LOGF("Led", LogLevel::LOG_INFO, "Initialized LED on pin %u", (unsigned)_pin);
  return true;
}

bool Led::on() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::HW, 2,
                         "Led: on() called before initialization");
    }
    LATTICE_LOGLN("Led", "ERROR: on() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  return setState(true);
}

bool Led::off() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::HW, 3,
                         "Led: off() called before initialization");
    }
    LATTICE_LOGLN("Led", "ERROR: off() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  return setState(false);
}

bool Led::toggle() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::HW, 4,
                         "Led: toggle() called before initialization");
    }
    LATTICE_LOGLN("Led", "ERROR: toggle() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  return setState(!_isOn);
}

bool Led::isOn() const {
  return _isOn;
}

// isInitialized is now inherited from GpioOutput

uint8_t Led::getPin() const {
  return _pin;
}

bool Led::setState(bool state) {
  if (_isOn == state)
    return true;
  gpio_set_level(static_cast<gpio_num_t>(_pin), state ? 1 : 0);
  _isOn = state;
  LATTICE_LOGF("Led", LogLevel::LOG_DEBUG, "LED on pin %u %s", (unsigned)_pin,
               state ? "ON" : "OFF");
  return true;
}

void Led::setSystemErrorLed(Led* led) {
  _systemErrorLed = led;
}

// Phase I Task 7 (WW): non-blocking replacement for the old blink(), which
// blocked the caller for the full pattern duration via delay(). pulse() arms
// the state machine and returns immediately; update(now_ms) — called every
// main-loop iteration — advances it. Reproduces the exact same timeline as
// the old blocking loop: ON for onMs, OFF for offMs, repeated `times` times,
// with no trailing OFF wait after the very last ON phase (matches the old
// `if (i < times - 1) delay(offTimeMs);` guard).
void Led::pulse(uint8_t times, uint32_t onMs, uint32_t offMs) {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::HW, 5,
                         "Led: pulse() called before initialization");
    }
    LATTICE_LOGLN("Led", "ERROR: pulse() called before initialization", LogLevel::LOG_ERROR);
    return;
  }
  if (times == 0) {
    _remaining = 0;
    return;
  }
  _onMs = onMs;
  _offMs = offMs;
  _remaining = times;
  _onPhase = true;
  setState(true);
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  _nextFlipMs = now + onMs;
  LATTICE_LOGF("Led", LogLevel::LOG_DEBUG, "Pulse pattern armed: %ux on pin %u", (unsigned)times,
               (unsigned)_pin);
}

void Led::update(uint64_t nowMs) {
  if (_remaining == 0)
    return;
  if (nowMs < _nextFlipMs)
    return;
  if (_onPhase) {
    setState(false);
    _onPhase = false;
    --_remaining;
    if (_remaining > 0) {
      _nextFlipMs = nowMs + _offMs;
    }
    // else: pattern finished — stays off, _remaining == 0 so update() is a
    // no-op from here until the next pulse() call.
  } else {
    setState(true);
    _onPhase = true;
    _nextFlipMs = nowMs + _onMs;
  }
}

bool Led::isBusy() const {
  return _remaining > 0;
}

} // namespace hardware
} // namespace lattice
