#include "GpioOutput.h"

namespace lattice {
namespace hardware {

GpioOutput::GpioOutput(uint8_t pin) : _pin(pin), _initialized(false) {}

bool GpioOutput::init() {
  if (!isValidOutputPin(_pin)) {
    return false;
  }
  // Phase I Task 7 (RR): per-init pinMode() call removed — GpioOutput-derived
  // pins are configured once at boot via main.cpp's bundled output-group
  // gpio_config_t call.
  _initialized = true;
  return true;
}

bool GpioOutput::isValidOutputPin(uint8_t pin) {
  // Bit N set means pin N is valid: 2,4,5,12-19,21-23,25-27,32-33 (audit item J — was a
  // 19-case switch). Narrower than GpioInput's mask — excludes input-only pin 0 and 34-39.
  constexpr uint64_t VALID_MASK = 0x30eeff034ULL;
  return pin < 64 && ((VALID_MASK >> pin) & 1) != 0;
}

bool GpioOutput::isInitialized() const {
  return _initialized;
}

} // namespace hardware
} // namespace lattice
