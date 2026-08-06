#ifndef PIR_H
#define PIR_H

#include "GpioInput.h"

namespace lattice {
namespace hardware {

class Pir : public GpioInput {
public:
  explicit Pir(uint8_t pin);
  ~Pir() = default;

  bool init();
  bool isMotionDetected() const;
  void clearMotion();

  // Adapter uses these to set up interrupts:
  bool attachInterrupt(void (*isr)(), int mode);
  void detachInterrupt();

  // Used in ISR:
  void signalMotion();

private:
  volatile bool _motionDetected;
  // Phase I Task 7 (QQ): the zero-arg callback attachInterrupt() was given —
  // invoked by isrTrampoline(), which native gpio_isr_handler_add() actually
  // registers (its handler signature takes a void* arg, not a zero-arg fn).
  void (*_isrCallback)() = nullptr;
  static void isrTrampoline(void* arg);
};

} // namespace hardware
} // namespace lattice
#endif
