#ifndef PIR_H
#define PIR_H

#include "GpioInput.h"

namespace lattice {
namespace hardware {

class Pir : public GpioInput {
public:
  explicit Pir(uint8_t pin);
  ~Pir() = default;

  bool init() override;
  bool isMotionDetected() const;
  void clearMotion();

  // Adapter uses these to set up interrupts:
  bool attachInterrupt(void (*isr)(), int mode);
  void detachInterrupt();

  // Used in ISR:
  void signalMotion();

private:
  volatile bool _motionDetected;
};

} // namespace hardware
} // namespace lattice
#endif
