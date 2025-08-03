#ifndef LED_H
#define LED_H
#include <Arduino.h>

namespace planetopia {
namespace hardware {

class Led {
public:
  explicit Led(uint8_t pin);
  ~Led();

  bool init();
  bool on();
  bool off();
  bool toggle();
  bool isOn() const;
  bool blink(uint8_t times = 1, unsigned int onTimeMs = 200, unsigned int offTimeMs = 200);
  bool isInitialized() const;

  uint8_t getPin() const;

  // For ErrorHandler: designate this instance as the error LED
  static void setSystemErrorLed(Led* led);

private:
  uint8_t _pin;
  bool _isOn;
  bool _initialized;

  bool isValidPin(uint8_t pin) const;
  bool setState(bool state);

  static Led* _systemErrorLed;  // Static pointer to the error handler's LED
};

}  // namespace hardware
}  // namespace planetopia

#endif