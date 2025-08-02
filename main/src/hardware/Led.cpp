#include "Led.h"

namespace planetopia {
namespace hardware {

Led::Led(uint8_t pin)
  : _pin(pin), _isOn(false), _initialized(false) {}

Led::~Led() {
  if (_initialized) {
    digitalWrite(_pin, LOW);
    pinMode(_pin, INPUT);
  }
}

bool Led::init() {
  // Validate pin, optionally
  pinMode(_pin, OUTPUT);
  digitalWrite(_pin, LOW);
  _isOn = false;
  _initialized = true;
  return true;
}

void Led::on() {
  if (_initialized) setState(true);
}

void Led::off() {
  if (_initialized) setState(false);
}

void Led::toggle() {
  if (_initialized) setState(!_isOn);
}

bool Led::isOn() const {
  return _isOn;
}

uint8_t Led::getPin() const {
  return _pin;
}

void Led::setState(bool state) {
  if (_isOn != state) {
    digitalWrite(_pin, state ? HIGH : LOW);
    _isOn = state;
  }
}

}
}
