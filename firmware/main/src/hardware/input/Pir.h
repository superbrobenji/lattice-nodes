#ifndef PIR_H
#define PIR_H

#include "GpioInput.h"
#include <esp_attr.h>

namespace lattice {
namespace hardware {

class Pir : public GpioInput {
public:
  explicit Pir(uint8_t pin);
  ~Pir() = default;

  bool init();
  bool isMotionDetected() const;
  void clearMotion();

  // Phase I Task 10: edge-mode selectors for attachInterrupt()'s `mode`
  // parameter below. This class's attachInterrupt() has called native
  // gpio_isr_handler_add()/gpio_set_intr_type() directly since Task 7 (QQ)
  // and never needed the rest of Arduino's interrupt machinery — only these
  // two symbolic edge values, previously Arduino.h's RISING/FALLING macros.
  // Deliberately not named RISING/FALLING themselves, to avoid a
  // macro-redefinition clash in translation units that still pull in
  // Arduino.h transitively via Logger.h (e.g. PirAdapter.cpp).
  static constexpr int kEdgeRising = 1;
  static constexpr int kEdgeFalling = 2;

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
  static void IRAM_ATTR isrTrampoline(void* arg);
};

} // namespace hardware
} // namespace lattice
#endif
