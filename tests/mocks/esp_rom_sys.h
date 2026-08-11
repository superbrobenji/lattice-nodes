// Mock esp_rom_sys.h — shadows the ESP-IDF ROM component header for host
// tests.
//
// Phase I Task 10 prereq cleanup: SevenSegDisplay.cpp's TM1637 bit-bang
// timing helper (tmDelay()) moved off Arduino's delayMicroseconds(3) onto
// the native esp_rom_delay_us(3). There is no real busy-wait needed on host
// builds (no physical bus to satisfy timing on), so this is a no-op — same
// as the delayMicroseconds() no-op it replaces in Arduino.h.
#pragma once
#include <cstdint>

inline void esp_rom_delay_us(uint32_t /*us*/) {}
