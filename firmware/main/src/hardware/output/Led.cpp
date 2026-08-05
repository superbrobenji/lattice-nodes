#include "Led.h"
#include "src/logging/Logger.h"
#include "src/error/Error.h"
#include <cstdio>

namespace lattice {
namespace hardware {

using namespace lattice::utils;

// Static member init
Led* Led::_systemErrorLed = nullptr;

// CONSTRUCTOR — now forwards pin to base GpioOutput!
Led::Led(uint8_t pin) : GpioOutput(pin), _isOn(false) {}

Led::~Led() {
  if (_initialized) {
    digitalWrite(_pin, LOW);
    pinMode(_pin, INPUT);
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
  digitalWrite(_pin, LOW);
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
  digitalWrite(_pin, state ? HIGH : LOW);
  _isOn = state;
  LATTICE_LOGF("Led", LogLevel::LOG_DEBUG, "LED on pin %u %s", (unsigned)_pin,
               state ? "ON" : "OFF");
  return true;
}

void Led::setSystemErrorLed(Led* led) {
  _systemErrorLed = led;
}

bool Led::blink(uint8_t times, unsigned int onTimeMs, unsigned int offTimeMs) {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::HW, 5,
                         "Led: blink() called before initialization");
    }
    LATTICE_LOGLN("Led", "ERROR: blink() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  for (uint8_t i = 0; i < times; ++i) {
    setState(true);
    delay(onTimeMs);
    setState(false);
    if (i < times - 1)
      delay(offTimeMs);
  }
  LATTICE_LOGF("Led", LogLevel::LOG_DEBUG, "Blink pattern: %ux on pin %u", (unsigned)times,
               (unsigned)_pin);
  return true;
}

} // namespace hardware
} // namespace lattice
