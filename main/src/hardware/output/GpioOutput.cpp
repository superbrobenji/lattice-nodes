#include "GpioOutput.h"

namespace planetopia {
namespace hardware {

GpioOutput::GpioOutput(uint8_t pin) : _pin(pin), _initialized(false) {}

bool GpioOutput::init() {
  if (!isValidOutputPin(_pin)) {
    return false;
  }
  pinMode(_pin, OUTPUT);
  _initialized = true;
  return true;
}

bool GpioOutput::isValidOutputPin(uint8_t pin) {
  switch (pin) {
  case 2:
  case 4:
  case 5:
  case 12:
  case 13:
  case 14:
  case 15:
  case 16:
  case 17:
  case 18:
  case 19:
  case 21:
  case 22:
  case 23:
  case 25:
  case 26:
  case 27:
  case 32:
  case 33:
    return true;
  default:
    return false;
  }
}

bool GpioOutput::isInitialized() const {
  return _initialized;
}

} // namespace hardware
} // namespace planetopia
