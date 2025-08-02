#pragma once

#include <Arduino.h>

namespace planetopia {
namespace hardware {

class Led {
public:
  explicit Led(uint8_t pin);
  ~Led();

  bool init();  // For hardware setup, returns success
  void on();
  void off();
  void toggle();
  bool isOn() const;
  uint8_t getPin() const;

private:
  uint8_t _pin;
  bool _isOn;
  bool _initialized;

  void setState(bool state);
};
}
}
