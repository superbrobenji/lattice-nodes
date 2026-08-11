#include "GpioInput.h"

namespace lattice {
namespace hardware {

GpioInput::GpioInput(uint8_t pin) : _pin(pin), _initialized(false) {}

bool GpioInput::init() {
  if (!isValidInputPin(_pin)) {
    return false;
  }
  // Phase I Task 7 (RR): per-init pinMode() call removed — GpioInput-derived
  // pins are configured once at boot via main.cpp's bundled input-group
  // gpio_config_t calls (pull-up or pull-down per pin, matching each
  // subclass's prior pinMode() choice).
  _initialized = true;
  return true;
}

bool GpioInput::isValidInputPin(uint8_t pin) {
  // Accept most GPIOs except strapping/flash pins and invalid ones. Bit N set means pin N is
  // valid: 0,2,4,5,12-19,21-23,25-27,32-36,39 (audit item J — was a 24-case switch).
  constexpr uint64_t VALID_MASK = 0x9f0eeff035ULL;
  return pin < 64 && ((VALID_MASK >> pin) & 1) != 0;
}

bool GpioInput::isInitialized() const {
  return _initialized;
}

} // namespace hardware
} // namespace lattice
