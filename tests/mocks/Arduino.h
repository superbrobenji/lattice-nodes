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
inline int  digitalRead(int)           { return 0; }
inline void digitalWrite(int, int)     {}
inline void pinMode(int, int)          {}
inline void analogWrite(int, int)      {}
inline void attachInterrupt(int, void(*)(), int) {}
inline void detachInterrupt(int)       {}
inline void yield()                    {}
inline void btStop()                   {}
inline uint32_t esp_random()           { return 42; }  // Deterministic for tests
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
  void restart() {}
  uint32_t getFreeHeap() { return 200000; }
};
extern ESPClass ESP;
