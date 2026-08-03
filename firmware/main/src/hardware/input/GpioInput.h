#ifndef GPIO_IN
#define GPIO_IN

#include <Arduino.h>

namespace lattice {
namespace hardware {

class GpioInput {
public:
  explicit GpioInput(uint8_t pin);
  virtual ~GpioInput() = default;

  virtual bool init();
  static bool isValidInputPin(uint8_t pin);
  bool isInitialized() const;

protected:
  uint8_t _pin;
  bool _initialized;
};

} // namespace hardware
} // namespace lattice

#endif
