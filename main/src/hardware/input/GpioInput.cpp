#include "GpioInput.h"

namespace planetopia {
namespace hardware {

GpioInput::GpioInput(uint8_t pin) : _pin(pin), _initialized(false) {}

bool GpioInput::init() {
  if (!isValidInputPin(_pin)) {
    return false;
  }
  pinMode(_pin, INPUT_PULLUP); // or INPUT, as needed by the hardware
  _initialized = true;
  return true;
}

bool GpioInput::isValidInputPin(uint8_t pin) {
  // Accept most GPIOs except strapping/flash pins and invalid
  switch (pin) {
  case 0:
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
  case 34:
  case 35:
  case 36:
  case 39:
    return true;
  default:
    return false;
  }
}

bool GpioInput::isInitialized() const {
  return _initialized;
}

} // namespace hardware
} // namespace planetopia
