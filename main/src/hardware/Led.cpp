#include "Led.h"
#include "src/utils/Logger.h"
#include "src/utils/ErrorHandler.h"

namespace planetopia {
namespace hardware {

using namespace planetopia::utils;

Led* Led::_systemErrorLed = nullptr;

Led::Led(uint8_t pin)
  : _pin(pin), _isOn(false), _initialized(false) {}

Led::~Led() {
  if (_initialized) {
    digitalWrite(_pin, LOW);
    pinMode(_pin, INPUT);
  }
}

bool Led::isValidPin(uint8_t pin) const {
  // ESP32: Most pins 0-39 are valid; refine as needed for your board
  return (pin <= 39);
}

bool Led::init() {
  if (!isValidPin(_pin)) {
    if (this != _systemErrorLed) {
      ErrorHandler::getInstance().signalError(
        ErrorType::CONFIG_ERROR,
        "Led: Invalid pin number");
    }
    return false;
  }
  pinMode(_pin, OUTPUT);
  digitalWrite(_pin, LOW);
  _isOn = false;
  _initialized = true;
  Logger::logln("Led", "Initialized LED on pin " + String(_pin));
  return true;
}

bool Led::on() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      ErrorHandler::getInstance().signalError(
        ErrorType::HARDWARE_FAILURE,
        "Led: on() called before initialization");
    }
    return false;
  }
  return setState(true);
}

bool Led::off() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      ErrorHandler::getInstance().signalError(
        ErrorType::HARDWARE_FAILURE,
        "Led: off() called before initialization");
    }
    return false;
  }
  return setState(false);
}

bool Led::blink(uint8_t times, unsigned int onTimeMs, unsigned int offTimeMs) {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      ErrorHandler::getInstance().signalError(
        ErrorType::HARDWARE_FAILURE,
        "Led: blink() called before initialization");
    }
    return false;
  }
  for (uint8_t i = 0; i < times; ++i) {
    setState(true);
    delay(onTimeMs);
    setState(false);
    if (i < times - 1) delay(offTimeMs);
  }
  return true;
}

bool Led::toggle() {
  if (!_initialized) {
    if (this != _systemErrorLed) {
      ErrorHandler::getInstance().signalError(
        ErrorType::HARDWARE_FAILURE,
        "Led: toggle() called before initialization");
    }
    return false;
  }
  return setState(!_isOn);
}

bool Led::isOn() const {
  return _isOn;
}

bool Led::isInitialized() const {
  return _initialized;
}

uint8_t Led::getPin() const {
  return _pin;
}

bool Led::setState(bool state) {
  if (_isOn == state) return true;
  digitalWrite(_pin, state ? HIGH : LOW);
  _isOn = state;
  return true;
}

void Led::setSystemErrorLed(Led* led) {
  _systemErrorLed = led;
}

}
}
