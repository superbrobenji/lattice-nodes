#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "time_mock.h"
#include "serial_mock.h"

// IRAM_ATTR is an ESP32 linker section attribute — no-op on host
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// GPIO stubs
// Mock digital pin state, settable via setMockDigitalRead() (used by Button
// debounce tests). Defaults to LOW (0) for every pin, matching the prior
// fixed-stub behavior for callers that never set anything.
constexpr int MOCK_DIGITAL_PIN_COUNT = 64;
extern int _mockDigitalPinState[MOCK_DIGITAL_PIN_COUNT];
inline int digitalRead(int pin) {
  return (pin >= 0 && pin < MOCK_DIGITAL_PIN_COUNT) ? _mockDigitalPinState[pin] : 0;
}
inline void setMockDigitalRead(int pin, int value) {
  if (pin >= 0 && pin < MOCK_DIGITAL_PIN_COUNT)
    _mockDigitalPinState[pin] = value;
}
// Reset all mock pin state back to LOW — call from test SetUp() so state
// doesn't leak between tests sharing the same process.
inline void resetMockDigitalPins() {
  for (int i = 0; i < MOCK_DIGITAL_PIN_COUNT; ++i)
    _mockDigitalPinState[i] = 0;
}
// Call counter for digitalWrite — used by DisplayManager tests to prove the
// tick() throttle (item S) skips display writes when nothing changed,
// without needing to decode the bit-banged 7-segment protocol itself.
extern int _mockDigitalWriteCallCount;
inline void digitalWrite(int, int)     { ++_mockDigitalWriteCallCount; }
inline void resetMockDigitalWriteCallCount() { _mockDigitalWriteCallCount = 0; }
inline void pinMode(int, int)          {}
inline void analogWrite(int, int)      {}
inline void attachInterrupt(int, void(*)(), int) {}
inline void detachInterrupt(int)       {}
inline void yield()                    {}
inline void btStop()                   {}
// Deterministic for tests. NOT inline: MasterBeacon.cpp (a FIRMWARE_SOURCES
// file linked into every test/e2e binary — see tests/CMakeLists.txt) reaches
// this declaration only via tests/mocks/esp_random.h (Phase C finding 17
// dropped Logger.h's transitive Arduino.h include, so MasterBeacon.cpp now
// includes the real <esp_random.h> directly instead). An `inline` definition
// here would only get emitted into a TU that both includes THIS header and
// calls the function — Arduino.cpp (below) does neither on its own, so nothing
// would ever emit the symbol and every such link would fail. Defining it as
// an ordinary function in Arduino.cpp instead guarantees exactly one
// always-emitted definition that both this header's and esp_random.h's
// declarations resolve to at link time.
uint32_t esp_random();
inline void delayMicroseconds(uint32_t) {}  // no-op in tests
inline int digitalPinToInterrupt(int pin) { return pin; }  // identity on host

#define INPUT         0
#define OUTPUT        1
#define INPUT_PULLDOWN 2
#define INPUT_PULLUP  3
#define HIGH          1
#define LOW           0
#define RISING        1
#define FALLING       2
#define CHANGE        3

// Print bases (used in String(value, HEX) calls)
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

// String class shim — wraps std::string and adds Arduino numeric constructors
#include <string>

class String : public std::string {
public:
  // Default / copy / move from std::string
  String() : std::string() {}
  String(const std::string& s) : std::string(s) {}
  String(std::string&& s) : std::string(std::move(s)) {}
  String(const char* s) : std::string(s ? s : "") {}
  String(char c) : std::string(1, c) {}

  // Numeric constructors (Arduino API)
  explicit String(int v, int base = 10) : std::string() {
    char buf[32];
    if (base == 16) snprintf(buf, sizeof(buf), "%X", v);
    else snprintf(buf, sizeof(buf), "%d", v);
    assign(buf);
  }
  explicit String(unsigned int v, int base = 10) : std::string() {
    char buf[32];
    if (base == 16) snprintf(buf, sizeof(buf), "%X", v);
    else snprintf(buf, sizeof(buf), "%u", v);
    assign(buf);
  }
  explicit String(long v, int base = 10) : std::string() {
    char buf[32];
    if (base == 16) snprintf(buf, sizeof(buf), "%lX", v);
    else snprintf(buf, sizeof(buf), "%ld", v);
    assign(buf);
  }
  explicit String(unsigned long v, int base = 10) : std::string() {
    char buf[32];
    if (base == 16) snprintf(buf, sizeof(buf), "%lX", v);
    else snprintf(buf, sizeof(buf), "%lu", v);
    assign(buf);
  }
  explicit String(uint8_t v, int base = 10) : String(static_cast<unsigned int>(v), base) {}
  explicit String(int8_t v, int base = 10) : String(static_cast<int>(v), base) {}
  explicit String(uint16_t v, int base = 10) : String(static_cast<unsigned int>(v), base) {}
  explicit String(int16_t v, int base = 10) : String(static_cast<int>(v), base) {}
  // uint32_t / int32_t constructors: only add when they differ from unsigned int / int
  // On macOS/LP64: uint32_t=unsigned int, int32_t=int so these would be redeclarations — omit them.
  // On ESP32/ILP32:  uint32_t=unsigned long, int32_t=long so they would be needed — but we're host-only.
  explicit String(double v, int decimals = 2) : std::string() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    assign(buf);
  }
  explicit String(float v, int decimals = 2) : String(static_cast<double>(v), decimals) {}

  // Allow construction from bool
  explicit String(bool v) : std::string(v ? "1" : "0") {}

  // += overloads matching Arduino API
  String& operator+=(const String& rhs) { std::string::operator+=(rhs); return *this; }
  String& operator+=(const char* rhs)   { std::string::operator+=(rhs); return *this; }
  String& operator+=(char c)            { std::string::operator+=(c); return *this; }

  // + operators
  friend String operator+(String lhs, const String& rhs) { lhs += rhs; return lhs; }
  friend String operator+(String lhs, const char* rhs)   { lhs += rhs; return lhs; }
  friend String operator+(const char* lhs, const String& rhs) { return String(lhs) + rhs; }

  const char* c_str() const { return std::string::c_str(); }
  size_t length() const { return std::string::length(); }
};

inline String String_from(int v) { return String(v); }

// ESP class stub
struct ESPClass {
  bool _restartRequested = false;
  void restart() { _restartRequested = true; }
  uint32_t getFreeHeap() { return 200000; }
};
extern ESPClass ESP;
