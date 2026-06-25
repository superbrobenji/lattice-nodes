#ifndef GPIO_OUT
#define GPIO_OUT

#include <Arduino.h>

namespace planetopia {
namespace hardware {

class GpioOutput {
public:
  explicit GpioOutput(uint8_t pin);
  virtual ~GpioOutput() = default;

  virtual bool init();
  static bool isValidOutputPin(uint8_t pin);
  bool isInitialized() const;

protected:
  uint8_t _pin;
  bool _initialized;
};

} // namespace hardware
} // namespace planetopia

#endif
