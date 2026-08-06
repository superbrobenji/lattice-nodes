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

  // Phase I Task 7 (WW): blink() replaced by a non-blocking pulse()/update()
  // state machine — blink() used to block the caller for the full pattern
  // duration via delay() (e.g. main.cpp's dataRecvCallback blocked every
  // ESP-NOW receive for ~300ms). pulse() arms the pattern and returns
  // immediately; update(now_ms) advances it and must be pumped regularly
  // (main loop() calls it every iteration).
  void pulse(uint8_t times = 1, uint32_t onMs = 200, uint32_t offMs = 200);
  void update(uint64_t nowMs);
  // True while a pulse() pattern is still in flight — lets callers that
  // genuinely need to wait for the pattern to finish (e.g. a pre-restart
  // confirmation blink) pump update() in a small local loop instead of the
  // old internal delay()-based blocking.
  bool isBusy() const;

  // Use GpioOutput::isInitialized() instead of a local _initialized
  using GpioOutput::isInitialized;

  uint8_t getPin() const;

  // For ErrorHandler: designate this instance as the error LED
  static void setSystemErrorLed(Led* led);

private:
  bool _isOn;

  // pulse()/update() state (Phase I Task 7 item WW).
  uint8_t _remaining = 0; // blink phases left to trigger; 0 == idle
  bool _onPhase = false;  // true while currently in the "on" half of a blink
  uint64_t _nextFlipMs = 0;
  uint32_t _onMs = 0, _offMs = 0;

  bool setState(bool state);

  static Led* _systemErrorLed; // Static pointer to the error handler's LED
};

} // namespace hardware
} // namespace lattice

#endif
