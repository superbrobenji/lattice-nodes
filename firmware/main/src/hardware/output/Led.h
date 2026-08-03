#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include "GpioOutput.h"

namespace lattice {
namespace hardware {

class Led : public GpioOutput {
public:
  explicit Led(uint8_t pin);
  ~Led();

  bool init();
  bool on();
  bool off();
  bool toggle();
  bool isOn() const;
  bool blink(uint8_t times = 1, unsigned int onTimeMs = 200, unsigned int offTimeMs = 200);

  // Use GpioOutput::isInitialized() instead of a local _initialized
  using GpioOutput::isInitialized;

  uint8_t getPin() const;

  // For ErrorHandler: designate this instance as the error LED
  static void setSystemErrorLed(Led* led);

private:
  bool _isOn;

  bool setState(bool state);

  static Led* _systemErrorLed; // Static pointer to the error handler's LED
};

} // namespace hardware
} // namespace lattice

#endif
