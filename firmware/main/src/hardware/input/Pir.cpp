#include "Pir.h"

namespace lattice {
namespace hardware {

Pir::Pir(uint8_t pin) : GpioInput(pin), _motionDetected(false) {}

bool Pir::init() {
  if (!GpioInput::init())
    return false;
  _motionDetected = false;
  return true;
}

bool Pir::isMotionDetected() const {
  return _motionDetected;
}

void Pir::clearMotion() {
  _motionDetected = false;
}

bool Pir::attachInterrupt(void (*isr)(), int mode) {
  if (!_initialized)
    return false;
  ::attachInterrupt(digitalPinToInterrupt(_pin), isr, mode);
  return true;
}

void Pir::detachInterrupt() {
  if (!_initialized)
    return;
  ::detachInterrupt(digitalPinToInterrupt(_pin));
}

void Pir::signalMotion() {
  _motionDetected = true;
}

} // namespace hardware
} // namespace lattice
