#include "Pir.h"
#include <driver/gpio.h>

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

// Phase I Task 7 (QQ): native gpio_isr_handler_add()'s handler signature is
// void(*)(void*) — this trampoline recovers the Pir instance from `arg` and
// forwards to whichever zero-arg callback attachInterrupt() was given (the
// public API is unchanged: callers still pass a plain void(*)()).
void IRAM_ATTR Pir::isrTrampoline(void* arg) {
  Pir* self = static_cast<Pir*>(arg);
  if (self && self->_isrCallback)
    self->_isrCallback();
}

bool Pir::attachInterrupt(void (*isr)(), int mode) {
  if (!_initialized)
    return false;
  gpio_int_type_t intrType = GPIO_INTR_ANYEDGE;
  if (mode == kEdgeRising)
    intrType = GPIO_INTR_POSEDGE;
  else if (mode == kEdgeFalling)
    intrType = GPIO_INTR_NEGEDGE;
  _isrCallback = isr;
  (void)gpio_set_intr_type(static_cast<gpio_num_t>(_pin), intrType);
  esp_err_t err = gpio_isr_handler_add(static_cast<gpio_num_t>(_pin), &Pir::isrTrampoline, this);
  if (err != ESP_OK) {
    _isrCallback = nullptr;
    return false;
  }
  (void)gpio_intr_enable(static_cast<gpio_num_t>(_pin));
  return true;
}

void Pir::detachInterrupt() {
  if (!_initialized)
    return;
  (void)gpio_intr_disable(static_cast<gpio_num_t>(_pin));
  (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(_pin));
  _isrCallback = nullptr;
}

void Pir::signalMotion() {
  _motionDetected = true;
}

} // namespace hardware
} // namespace lattice
