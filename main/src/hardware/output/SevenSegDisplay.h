#ifndef SEVENSEGDISPLAY_H
#define SEVENSEGDISPLAY_H

#include <Arduino.h>
#include "GpioOutput.h"

namespace planetopia {
namespace hardware {

class SevenSegDisplay {
public:
  SevenSegDisplay(uint8_t dioPin, uint8_t clkPin);
  bool init();

  // brightness 0 (low) – 7 (max)
  void setBrightness(uint8_t level);

  // Show a signed integer [-999, 9999]. If leadingZeros=false blank leading spaces.
  void show(int value, bool leadingZeros = true);

  // Low-level: write raw segment data (LSB=A, bit6=DP)
  void setSegments(const uint8_t segs[4]);

  void clear();

private:
  uint8_t _dioPin, _clkPin;
  uint8_t _brightness;

  void start();
  void stop();
  bool writeByte(uint8_t b);
  uint8_t encodeDigit(int d);
};

} // namespace hardware
} // namespace planetopia

#endif
