#include "Led.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"

namespace planetopia {
namespace hardware {

using namespace planetopia::utils;

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
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                            planetopia::core::ModuleDigit::HW, 1, "Led: Invalid pin number");
    }
    Logger::logln("Led", "ERROR: Invalid pin number for LED: " + String(_pin), LogLevel::LOG_ERROR);
    return false;
  }
  digitalWrite(_pin, LOW);
  _isOn = false;
  Logger::logln("Led", "Initialized LED on pin " + String(_pin), LogLevel::LOG_INFO);
  return true;
}

bool Led::on() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::HW, 2,
                            "Led: on() called before initialization");
    }
    Logger::logln("Led", "ERROR: on() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  return setState(true);
}

bool Led::off() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::HW, 3,
                            "Led: off() called before initialization");
    }
    Logger::logln("Led", "ERROR: off() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  return setState(false);
}

bool Led::toggle() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::HW, 4,
                            "Led: toggle() called before initialization");
    }
    Logger::logln("Led", "ERROR: toggle() called before initialization", LogLevel::LOG_ERROR);
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
  Logger::logln("Led", String("LED on pin ") + String(_pin) + (state ? " ON" : " OFF"),
                LogLevel::LOG_DEBUG);
  return true;
}

void Led::setSystemErrorLed(Led* led) {
  _systemErrorLed = led;
}

bool Led::blink(uint8_t times, unsigned int onTimeMs, unsigned int offTimeMs) {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                            planetopia::core::ModuleDigit::HW, 5,
                            "Led: blink() called before initialization");
    }
    Logger::logln("Led", "ERROR: blink() called before initialization", LogLevel::LOG_ERROR);
    return false;
  }
  for (uint8_t i = 0; i < times; ++i) {
    setState(true);
    delay(onTimeMs);
    setState(false);
    if (i < times - 1)
      delay(offTimeMs);
  }
  Logger::logln("Led", String("Blink pattern: ") + String(times) + "x on pin " + String(_pin),
                LogLevel::LOG_DEBUG);
  return true;
}

} // namespace hardware
} // namespace planetopia
