#include "SevenSegDisplay.h"
#include "GpioOutput.h"
#include "src/core/Logger.h"
#include "src/error/Error.h"

using planetopia::core::ErrorTypeDigit;
using planetopia::core::ModuleDigit;
using planetopia::utils::Logger;

namespace planetopia {
namespace hardware {

// 0b0GFEDCBA – bit7 is DP
static const uint8_t FONT[16] = {
    0b00111111, // 0
    0b00000110, // 1
    0b01011011, // 2
    0b01001111, // 3
    0b01100110, // 4
    0b01101101, // 5
    0b01111101, // 6
    0b00000111, // 7
    0b01111111, // 8
    0b01101111, // 9
    0b01110111, // A
    0b01111100, // b
    0b00111001, // C
    0b01011110, // d
    0b01111001, // E
    0b01110001  // F
};

SevenSegDisplay::SevenSegDisplay(uint8_t dio, uint8_t clk)
    : _dioPin(dio), _clkPin(clk), _brightness(7) {}

bool SevenSegDisplay::init() {
  if (!GpioOutput::isValidOutputPin(_dioPin) || !GpioOutput::isValidOutputPin(_clkPin)) {
    Logger::logln("7SEG", "Invalid GPIO pins", planetopia::utils::LogLevel::LOG_ERROR);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::CONFIG,
                          planetopia::core::ModuleDigit::HW, 1, "7Seg invalid pins");
    return false;
  }
  pinMode(_dioPin, OUTPUT);
  pinMode(_clkPin, OUTPUT);
  digitalWrite(_clkPin, HIGH);
  digitalWrite(_dioPin, HIGH);
  Logger::logln("7SEG", "SevenSegDisplay initialized", planetopia::utils::LogLevel::LOG_INFO);
  // Self-test: flash all segments 0x7F (88:88)
  uint8_t testSeg[4] = {0x7F, 0x7F, 0x7F, 0x7F};
  setSegments(testSeg);
  delay(500);
  clear();
  return true;
}

void SevenSegDisplay::clear() {
  uint8_t blank[4] = {0, 0, 0, 0};
  setSegments(blank);
}

void SevenSegDisplay::setBrightness(uint8_t level) {
  _brightness = level & 0x07;
}

// timing helper
static inline void tmDelay() {
  delayMicroseconds(3);
}

void SevenSegDisplay::start() {
  digitalWrite(_dioPin, HIGH);
  digitalWrite(_clkPin, HIGH);
  tmDelay();
  digitalWrite(_dioPin, LOW);
  tmDelay();
  digitalWrite(_clkPin, LOW);
}

void SevenSegDisplay::stop() {
  digitalWrite(_clkPin, LOW);
  tmDelay();
  digitalWrite(_dioPin, LOW);
  tmDelay();
  digitalWrite(_clkPin, HIGH);
  tmDelay();
  digitalWrite(_dioPin, HIGH);
  tmDelay();
}

bool SevenSegDisplay::writeByte(uint8_t b) {
  for (int i = 0; i < 8; ++i) {
    digitalWrite(_clkPin, LOW);
    digitalWrite(_dioPin, (b & 0x01) ? HIGH : LOW);
    tmDelay();
    digitalWrite(_clkPin, HIGH);
    tmDelay();
    b >>= 1;
  }
  // Wait for ACK
  digitalWrite(_clkPin, LOW);
  pinMode(_dioPin, INPUT);
  digitalWrite(_dioPin, HIGH); // pull-up
  tmDelay();
  digitalWrite(_clkPin, HIGH);
  tmDelay();

  unsigned long start = millis();
  bool ack = false;
  while (millis() - start < 20) {
    if (!digitalRead(_dioPin)) {
      ack = true;
      break;
    }
  }
  pinMode(_dioPin, OUTPUT);
  digitalWrite(_clkPin, LOW);
  if (!ack) {
    Logger::logln("7SEG", "ACK timeout", planetopia::utils::LogLevel::LOG_WARN);
    planetopia::err::fail(planetopia::core::ErrorTypeDigit::HARDWARE,
                          planetopia::core::ModuleDigit::HW, 2, "7Seg ACK timeout");
  }
  return ack;
}

uint8_t SevenSegDisplay::encodeDigit(int d) {
  if (d >= 0 && d < 16)
    return FONT[d];
  if (d == -1)
    return 0x40; // minus sign on segment G
  return 0x00;
}

void SevenSegDisplay::setSegments(const uint8_t segs[4]) {
  start();
  writeByte(0x40); // automatic address increment mode
  stop();

  start();
  writeByte(0xC0); // starting address 0
  for (int i = 0; i < 4; ++i)
    writeByte(segs[i]);
  stop();

  start();
  writeByte(0x88 | _brightness);
  stop();
}

void SevenSegDisplay::show(int value, bool leadingZeros) {
  bool negative = value < 0;
  int v = negative ? -value : value;
  if (v > 9999)
    v = 9999;

  int digits[4];
  for (int i = 3; i >= 0; --i) {
    digits[i] = v % 10;
    v /= 10;
  }

  if (negative) {
    // show minus on leftmost non-zero digit position
    for (int i = 0; i < 4; ++i) {
      if (digits[i] != 0 || i == 3) { // last digit
        digits[i] = -1;               // minus
        break;
      }
    }
  }

  uint8_t segs[4];
  for (int i = 0; i < 4; ++i) {
    if (!leadingZeros && digits[i] == 0 && i < 3 && !negative)
      segs[i] = 0x00;
    else
      segs[i] = encodeDigit(digits[i]);
  }
  setSegments(segs);
}

} // namespace hardware
} // namespace planetopia
